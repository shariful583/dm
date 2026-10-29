// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

namespace lanes::gui {

// Live speed graph of the progress window: an area chart of the last samples (oldest left),
// scaled to the highest one. GUI thread only.
class SpeedGraph : public QQuickPaintedItem {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QVariantList samples READ samples WRITE setSamples NOTIFY changed) // bytes/s
  Q_PROPERTY(int capacity MEMBER capacity_ NOTIFY changed) // samples across the full width
  Q_PROPERTY(QColor lineColor MEMBER line_ NOTIFY changed)
  Q_PROPERTY(QColor gridColor MEMBER grid_ NOTIFY changed)

public:
  explicit SpeedGraph(QQuickItem *parent = nullptr);
  void paint(QPainter *painter) override;

  QVariantList samples() const { return samples_; }
  void setSamples(const QVariantList &samples);

signals:
  void changed();

private:
  QVariantList samples_;
  int capacity_ = 120;
  QColor line_{0, 103, 192};
  QColor grid_{0, 0, 0, 25};
};

} // namespace lanes::gui
