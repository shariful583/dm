// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>
#include <set>
#include <vector>

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "core/engine/engine.hpp"

namespace lanes::gui {

// Every download of the engine, as list rows for QML. Engine events arrive on engine threads and
// only mark rows dirty; a 250 ms timer on the GUI thread applies them (progress updates reach
// QML at most 4 times a second).
//
// Thread safety: lives on the GUI thread; only the engine callback touches the dirty set (locked).
class DownloadModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("provided by App")
  Q_PROPERTY(int activeCount READ activeCount NOTIFY statsChanged)
  Q_PROPERTY(double totalSpeed READ totalSpeed NOTIFY statsChanged)
  // Downloaded / total of the running downloads with a known size (0-1), -1 if there are none.
  Q_PROPERTY(double overallProgress READ overallProgress NOTIFY statsChanged)
  // Row counts per sidebar view ("all", "unfinished", "finished", "category:Video", "queue:main").
  Q_PROPERTY(QVariantMap counts READ counts NOTIFY statsChanged)

public:
  enum Role {
    IdRole = Qt::UserRole + 1,
    FileNameRole,
    UrlRole,
    SizeRole,       // bytes, -1 = unknown
    DownloadedRole, // bytes
    ProgressRole,   // 0-1, -1 = unknown
    SpeedRole,      // bytes/s while running, else 0
    TimeLeftRole,   // seconds, -1 = unknown
    StateRole, // QML "status": "queued", "scheduled", "running", "paused", "completed", "failed",
               // ...
    CategoryRole,
    QueueRole,
    ErrorRole,
    AddedRole,     // QDateTime
    CompletedRole, // QDateTime, invalid until complete
    StartAtRole,   // QDateTime, invalid if not scheduled
    FolderRole,    // native path, "" until known
    FilePathRole,  // native path of the final file, "" until known
    ConnectionsRole,
    SpeedLimitRole, // bytes/s, 0 = none
  };
  Q_ENUM(Role)

  explicit DownloadModel(core::Engine &engine, QObject *parent = nullptr);
  ~DownloadModel() override;

  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QHash<int, QByteArray> roleNames() const override;

  // Applies pending engine events now (the timer calls it every 250 ms).
  void refresh();

  const core::DownloadInfo *find(core::DownloadId id) const;

  int activeCount() const { return active_; }
  double totalSpeed() const { return speed_; }
  double overallProgress() const { return progress_; }
  QVariantMap counts() const { return counts_; }

signals:
  void statsChanged();
  // A download moved from one state to another ("running" → "completed", ...).
  void stateChanged(qint64 id, const QString &from, const QString &to);
  // Emitted after every refresh that changed something (progress windows re-read details).
  void refreshed();

private:
  void update_stats();

  core::Engine &engine_;
  int subscription_ = 0;
  std::mutex mutex_; // guards dirty_
  std::set<core::DownloadId> dirty_;
  std::vector<core::DownloadInfo> rows_;
  QTimer timer_;
  int active_ = 0;
  double speed_ = 0;
  double progress_ = -1;
  QVariantMap counts_;
};

// The list as shown: one sidebar view, the search text, and a sort order.
class DownloadFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  QML_ELEMENT
  QML_UNCREATABLE("provided by App")
  // "all", "unfinished", "finished", "category:<name>", "queue:<name>"
  Q_PROPERTY(QString view READ view WRITE setView NOTIFY viewChanged)
  Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
  // "name", "size", "status", "timeLeft", "speed", "added"
  Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
  Q_PROPERTY(bool descending READ descending WRITE setDescending NOTIFY sortChanged)
  Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
  explicit DownloadFilterModel(DownloadModel *source, QObject *parent = nullptr);

  QString view() const { return view_; }
  void setView(const QString &view);
  QString search() const { return search_; }
  void setSearch(const QString &search);
  QString sortKey() const { return sort_key_; }
  void setSortKey(const QString &key);
  bool descending() const { return descending_; }
  void setDescending(bool descending);
  int count() const { return rowCount(); }

  Q_INVOKABLE qint64 idAt(int row) const;
  Q_INVOKABLE int rowOf(qint64 id) const;
  // Ids of rows first..last (either order), for shift-click selection.
  Q_INVOKABLE QVariantList idsBetween(int first, int last) const;
  Q_INVOKABLE QVariantList allIds() const;

signals:
  void viewChanged();
  void searchChanged();
  void sortChanged();
  void countChanged();

protected:
  bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
  bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
  void apply_sort();
  template <typename Change> void change_filter(Change change);

  QString view_ = QStringLiteral("all");
  QString search_;
  QString sort_key_ = QStringLiteral("added");
  bool descending_ = true;
};

} // namespace lanes::gui
