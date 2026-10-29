// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/speed_graph.hpp"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>

namespace lanes::gui {

SpeedGraph::SpeedGraph(QQuickItem *parent) : QQuickPaintedItem(parent) {
  setAntialiasing(true);
  connect(this, &SpeedGraph::changed, this, [this] { update(); });
}

void SpeedGraph::setSamples(const QVariantList &samples) {
  samples_ = samples;
  emit changed();
}

void SpeedGraph::paint(QPainter *painter) {
  const QRectF r = boundingRect().adjusted(0.5, 0.5, -0.5, -0.5);
  painter->setPen(QPen(grid_, 1));
  for (int i = 1; i < 4; ++i) {
    const qreal y = std::round(r.top() + r.height() * i / 4) + 0.5;
    painter->drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
  }
  if (samples_.size() < 2 || capacity_ < 2) {
    return;
  }
  double top = 0;
  for (const QVariant &v : samples_) {
    top = std::max(top, v.toDouble());
  }
  if (top <= 0) {
    return;
  }
  top *= 1.15; // headroom above the peak

  const qsizetype n = std::min<qsizetype>(samples_.size(), capacity_);
  const qsizetype first = samples_.size() - n;
  const qreal step = r.width() / (capacity_ - 1);
  const qreal x0 = r.right() - step * static_cast<qreal>(n - 1);
  QPainterPath line;
  for (qsizetype i = 0; i < n; ++i) {
    const double v = samples_[first + i].toDouble();
    const QPointF p(x0 + step * static_cast<qreal>(i), r.bottom() - r.height() * (v / top));
    i == 0 ? line.moveTo(p) : line.lineTo(p);
  }
  QPainterPath area = line;
  area.lineTo(r.right(), r.bottom());
  area.lineTo(x0, r.bottom());
  area.closeSubpath();
  QLinearGradient fill(r.topLeft(), r.bottomLeft());
  QColor c = line_;
  c.setAlphaF(0.35f);
  fill.setColorAt(0, c);
  c.setAlphaF(0.04f);
  fill.setColorAt(1, c);
  painter->fillPath(area, fill);
  painter->setPen(QPen(line_, 1.5));
  painter->drawPath(line);
}

} // namespace lanes::gui
