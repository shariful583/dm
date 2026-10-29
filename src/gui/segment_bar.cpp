// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/segment_bar.hpp"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>

namespace lanes::gui {

SegmentBar::SegmentBar(QQuickItem *parent) : QQuickPaintedItem(parent) {
  setAntialiasing(true);
  connect(this, &SegmentBar::changed, this, [this] { update(); });
}

void SegmentBar::setSegments(const QVariantList &segments) {
  segments_ = segments;
  emit changed();
}

void SegmentBar::setTotal(double total) {
  if (total_ != total) {
    total_ = total;
    emit changed();
  }
}

void SegmentBar::setComplete(bool complete) {
  if (complete_ != complete) {
    complete_ = complete;
    emit changed();
  }
}

void SegmentBar::paint(QPainter *painter) {
  const QRectF r = boundingRect();
  const qreal radius = std::min<qreal>(4, r.height() / 2);
  QPainterPath clip;
  clip.addRoundedRect(r, radius, radius);
  painter->setClipPath(clip);
  painter->fillRect(r, track_);

  if (complete_) {
    painter->fillRect(r, fill_);
    return;
  }
  if (total_ <= 0) {
    return;
  }
  const auto x_of = [&](double byte) {
    return r.left() + std::clamp(byte / total_, 0.0, 1.0) * r.width();
  };
  for (const QVariant &v : segments_) {
    const QVariantMap s = v.toMap();
    const double start = s.value(QStringLiteral("start")).toDouble();
    const double received = s.value(QStringLiteral("received")).toDouble();
    const qreal x0 = x_of(start);
    const qreal x1 = x_of(start + received);
    if (x1 > x0) {
      painter->fillRect(QRectF(x0, r.top(), std::max<qreal>(x1 - x0, 1), r.height()), fill_);
    }
  }
  // Range borders: where each connection starts.
  painter->setPen(QPen(border_, 1));
  for (const QVariant &v : segments_) {
    const double start = v.toMap().value(QStringLiteral("start")).toDouble();
    if (start > 0) {
      const qreal x = std::round(x_of(start)) + 0.5;
      painter->drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
  }
}

} // namespace lanes::gui
