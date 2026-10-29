// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/download_model.hpp"

#include <algorithm>

#include <QDateTime>
#include <QDir>

#include "core/platform/path.hpp"

namespace lanes::gui {

namespace {

using core::DownloadState;

QString native_path(const std::filesystem::path &path) {
  return path.empty()
             ? QString()
             : QDir::toNativeSeparators(QString::fromStdString(core::platform::path_to_utf8(path)));
}

QDateTime from_unix(std::optional<std::int64_t> seconds) {
  return seconds ? QDateTime::fromSecsSinceEpoch(*seconds) : QDateTime();
}

double time_left(const core::DownloadInfo &d) {
  if (d.state != DownloadState::Running || !d.size || d.bytes_per_second <= 0) {
    return -1;
  }
  return static_cast<double>(*d.size - std::min(*d.size, d.downloaded)) / d.bytes_per_second;
}

// Order for sorting by status: active work first, finished last.
int state_rank(DownloadState s) {
  switch (s) {
  case DownloadState::Running:
    return 0;
  case DownloadState::Queued:
    return 1;
  case DownloadState::Scheduled:
    return 2;
  case DownloadState::Paused:
    return 3;
  case DownloadState::NeedsAddress:
    return 4;
  case DownloadState::Failed:
    return 5;
  case DownloadState::Completed:
    return 6;
  }
  return 7;
}

} // namespace

DownloadModel::DownloadModel(core::Engine &engine, QObject *parent)
    : QAbstractListModel(parent), engine_(engine), rows_(engine.list()) {
  subscription_ = engine_.subscribe([this](const core::EngineEvent &e) {
    std::lock_guard lock(mutex_);
    dirty_.insert(e.id);
  });
  timer_.setInterval(250);
  connect(&timer_, &QTimer::timeout, this, &DownloadModel::refresh);
  timer_.start();
  update_stats();
}

DownloadModel::~DownloadModel() {
  engine_.unsubscribe(subscription_); // waits for a callback that is running
}

int DownloadModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DownloadModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
    return {};
  }
  const core::DownloadInfo &d = rows_[static_cast<std::size_t>(index.row())];
  switch (role) {
  case Qt::DisplayRole:
  case FileNameRole:
    return QString::fromStdString(d.filename);
  case IdRole:
    return static_cast<qint64>(d.id);
  case UrlRole:
    return QString::fromStdString(d.url);
  case SizeRole:
    return d.size ? static_cast<double>(*d.size) : -1.0;
  case DownloadedRole:
    return static_cast<double>(d.downloaded);
  case ProgressRole:
    if (d.state == DownloadState::Completed) {
      return 1.0;
    }
    return d.size && *d.size > 0
               ? std::min(1.0, static_cast<double>(d.downloaded) / static_cast<double>(*d.size))
               : -1.0;
  case SpeedRole:
    return d.state == DownloadState::Running ? d.bytes_per_second : 0.0;
  case TimeLeftRole:
    return time_left(d);
  case StateRole:
    return QString::fromUtf8(core::to_string(d.state));
  case CategoryRole:
    return QString::fromStdString(d.category);
  case QueueRole:
    return QString::fromStdString(d.queue);
  case ErrorRole:
    return QString::fromStdString(d.error);
  case AddedRole:
    return QDateTime::fromSecsSinceEpoch(d.added_at);
  case CompletedRole:
    return from_unix(d.completed_at);
  case StartAtRole:
    return from_unix(d.state == DownloadState::Scheduled ? d.start_at : std::nullopt);
  case FolderRole:
    return native_path(d.directory);
  case FilePathRole:
    return d.directory.empty() || d.filename.empty()
               ? QString()
               : native_path(d.directory / core::platform::path_from_utf8(d.filename));
  case ConnectionsRole:
    return d.connections;
  case SpeedLimitRole:
    return static_cast<double>(d.speed_limit);
  default:
    return {};
  }
}

QHash<int, QByteArray> DownloadModel::roleNames() const {
  return {
      {IdRole, "downloadId"},
      {FileNameRole, "fileName"},
      {UrlRole, "url"},
      {SizeRole, "size"},
      {DownloadedRole, "downloaded"},
      {ProgressRole, "progress"},
      {SpeedRole, "speed"},
      {TimeLeftRole, "timeLeft"},
      {StateRole, "status"},
      {CategoryRole, "category"},
      {QueueRole, "queue"},
      {ErrorRole, "error"},
      {AddedRole, "added"},
      {CompletedRole, "completed"},
      {StartAtRole, "startAt"},
      {FolderRole, "folder"},
      {FilePathRole, "filePath"},
      {ConnectionsRole, "connections"},
      {SpeedLimitRole, "speedLimit"},
  };
}

void DownloadModel::refresh() {
  std::set<core::DownloadId> dirty;
  {
    std::lock_guard lock(mutex_);
    dirty.swap(dirty_);
  }
  if (dirty.empty()) {
    return;
  }
  for (const core::DownloadId id : dirty) {
    const auto it = std::find_if(rows_.begin(), rows_.end(),
                                 [id](const core::DownloadInfo &d) { return d.id == id; });
    const int row = it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
    auto info = engine_.get(id);
    if (!info) {
      if (row >= 0) {
        beginRemoveRows({}, row, row);
        rows_.erase(it);
        endRemoveRows();
      }
      continue;
    }
    if (row < 0) {
      beginInsertRows({}, rowCount(), rowCount());
      rows_.push_back(std::move(*info));
      endInsertRows();
      continue;
    }
    const DownloadState before = it->state;
    *it = std::move(*info);
    emit dataChanged(index(row), index(row));
    if (before != it->state) {
      emit stateChanged(static_cast<qint64>(id), QString::fromUtf8(core::to_string(before)),
                        QString::fromUtf8(core::to_string(it->state)));
    }
  }
  update_stats();
  emit refreshed();
}

const core::DownloadInfo *DownloadModel::find(core::DownloadId id) const {
  const auto it = std::find_if(rows_.begin(), rows_.end(),
                               [id](const core::DownloadInfo &d) { return d.id == id; });
  return it == rows_.end() ? nullptr : &*it;
}

void DownloadModel::update_stats() {
  int active = 0;
  double speed = 0;
  std::uint64_t done = 0;
  std::uint64_t total = 0;
  QVariantMap counts;
  const auto bump = [&counts](const QString &key) { counts[key] = counts.value(key).toInt() + 1; };
  for (const auto &d : rows_) {
    bump(QStringLiteral("all"));
    bump(d.state == DownloadState::Completed ? QStringLiteral("finished")
                                             : QStringLiteral("unfinished"));
    bump(QStringLiteral("category:") + QString::fromStdString(d.category));
    if (!d.queue.empty()) {
      bump(QStringLiteral("queue:") + QString::fromStdString(d.queue));
    }
    if (d.state == DownloadState::Running) {
      ++active;
      speed += d.bytes_per_second;
      if (d.size) {
        done += std::min(d.downloaded, *d.size);
        total += *d.size;
      }
    }
  }
  active_ = active;
  speed_ = speed;
  progress_ = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : -1.0;
  counts_ = counts;
  emit statsChanged();
}

// --- DownloadFilterModel ---

DownloadFilterModel::DownloadFilterModel(DownloadModel *source, QObject *parent)
    : QSortFilterProxyModel(parent) {
  setSourceModel(source);
  setDynamicSortFilter(true);
  connect(this, &QAbstractItemModel::rowsInserted, this, &DownloadFilterModel::countChanged);
  connect(this, &QAbstractItemModel::rowsRemoved, this, &DownloadFilterModel::countChanged);
  connect(this, &QAbstractItemModel::modelReset, this, &DownloadFilterModel::countChanged);
  connect(this, &QAbstractItemModel::layoutChanged, this, &DownloadFilterModel::countChanged);
  apply_sort();
}

template <typename Change> void DownloadFilterModel::change_filter(Change change) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  beginFilterChange();
  change();
  endFilterChange(Direction::Rows);
#else
  change();
  invalidateRowsFilter();
#endif
}

void DownloadFilterModel::setView(const QString &view) {
  if (view_ != view) {
    change_filter([&] { view_ = view; });
    emit viewChanged();
    emit countChanged();
  }
}

void DownloadFilterModel::setSearch(const QString &search) {
  if (search_ != search) {
    change_filter([&] { search_ = search; });
    emit searchChanged();
    emit countChanged();
  }
}

void DownloadFilterModel::setSortKey(const QString &key) {
  if (sort_key_ != key) {
    sort_key_ = key;
    apply_sort();
    emit sortChanged();
  }
}

void DownloadFilterModel::setDescending(bool descending) {
  if (descending_ != descending) {
    descending_ = descending;
    apply_sort();
    emit sortChanged();
  }
}

void DownloadFilterModel::apply_sort() {
  invalidate();
  sort(0, descending_ ? Qt::DescendingOrder : Qt::AscendingOrder);
}

qint64 DownloadFilterModel::idAt(int row) const {
  return data(index(row, 0), DownloadModel::IdRole).toLongLong();
}

int DownloadFilterModel::rowOf(qint64 id) const {
  for (int row = 0; row < rowCount(); ++row) {
    if (idAt(row) == id) {
      return row;
    }
  }
  return -1;
}

QVariantList DownloadFilterModel::idsBetween(int first, int last) const {
  QVariantList ids;
  for (int row = std::max(0, std::min(first, last));
       row <= std::max(first, last) && row < rowCount(); ++row) {
    ids.push_back(idAt(row));
  }
  return ids;
}

QVariantList DownloadFilterModel::allIds() const { return idsBetween(0, rowCount() - 1); }

bool DownloadFilterModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const {
  const QModelIndex i = sourceModel()->index(source_row, 0, source_parent);
  const auto value = [&i](int role) { return i.data(role); };
  const QString state = value(DownloadModel::StateRole).toString();
  if (view_ == QLatin1StringView("finished") && state != QLatin1StringView("completed")) {
    return false;
  }
  if (view_ == QLatin1StringView("unfinished") && state == QLatin1StringView("completed")) {
    return false;
  }
  if (view_.startsWith(QLatin1StringView("category:")) &&
      value(DownloadModel::CategoryRole).toString() != view_.mid(9)) {
    return false;
  }
  if (view_.startsWith(QLatin1StringView("queue:")) &&
      value(DownloadModel::QueueRole).toString() != view_.mid(6)) {
    return false;
  }
  if (!search_.isEmpty() &&
      !value(DownloadModel::FileNameRole).toString().contains(search_, Qt::CaseInsensitive) &&
      !value(DownloadModel::UrlRole).toString().contains(search_, Qt::CaseInsensitive)) {
    return false;
  }
  return true;
}

bool DownloadFilterModel::lessThan(const QModelIndex &left, const QModelIndex &right) const {
  const auto l = [&left](int role) { return left.data(role); };
  const auto r = [&right](int role) { return right.data(role); };
  if (sort_key_ == QLatin1StringView("name")) {
    return QString::localeAwareCompare(l(DownloadModel::FileNameRole).toString(),
                                       r(DownloadModel::FileNameRole).toString()) < 0;
  }
  if (sort_key_ == QLatin1StringView("size")) {
    return l(DownloadModel::SizeRole).toDouble() < r(DownloadModel::SizeRole).toDouble();
  }
  if (sort_key_ == QLatin1StringView("speed")) {
    return l(DownloadModel::SpeedRole).toDouble() < r(DownloadModel::SpeedRole).toDouble();
  }
  if (sort_key_ == QLatin1StringView("timeLeft")) {
    return l(DownloadModel::TimeLeftRole).toDouble() < r(DownloadModel::TimeLeftRole).toDouble();
  }
  if (sort_key_ == QLatin1StringView("status")) {
    const auto rank = [](const QVariant &v) {
      const auto s = core::parse_download_state(v.toString().toStdString());
      return s ? state_rank(*s) : 7;
    };
    const int a = rank(l(DownloadModel::StateRole));
    const int b = rank(r(DownloadModel::StateRole));
    if (a != b) {
      return a < b;
    }
    return l(DownloadModel::ProgressRole).toDouble() < r(DownloadModel::ProgressRole).toDouble();
  }
  // "added"; ties (same second) by id, so the order is stable.
  const auto la = l(DownloadModel::AddedRole).toDateTime();
  const auto ra = r(DownloadModel::AddedRole).toDateTime();
  if (la != ra) {
    return la < ra;
  }
  return l(DownloadModel::IdRole).toLongLong() < r(DownloadModel::IdRole).toLongLong();
}

} // namespace lanes::gui
