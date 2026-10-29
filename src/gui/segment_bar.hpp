// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace lanes::gui {

// IDM's connection map: the whole file as a bar, each connection's received bytes filled in from
// its range start, range borders marked. GUI thread only.
class SegmentBar : public QQuickPaintedItem {
  Q_OBJECT
  QML_ELEMENT
  // [{start, end (inclusive, -1 = open), received}], as App.details() returns them.
  Q_PROPERTY(QVariantList segments READ segments WRITE setSegments NOTIFY changed)
  Q_PROPERTY(double total READ total WRITE setTotal NOTIFY changed)
  Q_PROPERTY(bool complete READ complete WRITE setComplete NOTIFY changed)
  Q_PROPERTY(QColor barColor MEMBER fill_ NOTIFY changed)
  Q_PROPERTY(QColor trackColor MEMBER track_ NOTIFY changed)
  Q_PROPERTY(QColor borderColor MEMBER border_ NOTIFY changed)

public:
  explicit SegmentBar(QQuickItem *parent = nullptr);
  void paint(QPainter *painter) override;

  QVariantList segments() const { return segments_; }
  void setSegments(const QVariantList &segments);
  double total() const { return total_; }
  void setTotal(double total);
  bool complete() const { return complete_; }
  void setComplete(bool complete);

signals:
  void changed();

private:
  QVariantList segments_;
  double total_ = 0;
  bool complete_ = false;
  QColor fill_{0, 103, 192};
  QColor track_{0, 0, 0, 20};
  QColor border_{0, 0, 0, 90};
};

} // namespace lanes::gui
