#include "gui/appicon.h"

#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

QIcon createAppIcon(int unreadCount) {
  QIcon icon;
  for (const int size : {16, 24, 32, 48, 64, 128}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 64.0, size / 64.0);

    QLinearGradient background(3, 3, 61, 61);
    background.setColorAt(0, QColor(QStringLiteral("#17243d")));
    background.setColorAt(1, QColor(QStringLiteral("#334978")));
    painter.setPen(QPen(QColor(255, 255, 255, 38), 1.2));
    painter.setBrush(background);
    painter.drawRoundedRect(QRectF(2.5, 2.5, 59, 59), 17, 17);

    QPainterPath rearBubble;
    rearBubble.addRoundedRect(QRectF(29, 29, 25, 20), 7, 7);
    rearBubble.moveTo(44, 46);
    rearBubble.lineTo(48, 53);
    rearBubble.lineTo(50, 46);
    QLinearGradient rearGradient(29, 29, 54, 49);
    rearGradient.setColorAt(0, QColor(QStringLiteral("#b59bff")));
    rearGradient.setColorAt(1, QColor(QStringLiteral("#8069e8")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(rearGradient);
    painter.drawPath(rearBubble);

    QPainterPath frontBubble;
    frontBubble.addRoundedRect(QRectF(9, 11, 39, 33), 10, 10);
    frontBubble.moveTo(18, 40);
    frontBubble.lineTo(13, 51);
    frontBubble.lineTo(28, 41);
    QLinearGradient frontGradient(10, 12, 47, 44);
    frontGradient.setColorAt(0, QColor(QStringLiteral("#62f0d0")));
    frontGradient.setColorAt(1, QColor(QStringLiteral("#17bcae")));
    painter.setBrush(frontGradient);
    painter.drawPath(frontBubble);

    painter.setBrush(QColor(QStringLiteral("#12344a")));
    for (const qreal x : {20.0, 28.5, 37.0}) {
      painter.drawEllipse(QPointF(x, 27.5), 2.4, 2.4);
    }

    if (unreadCount > 0) {
      painter.setPen(QPen(QColor(QStringLiteral("#17243d")), 2.2));
      painter.setBrush(QColor(QStringLiteral("#ff5268")));
      painter.drawEllipse(QPointF(51, 13), 11, 11);
      if (size >= 48) {
        painter.setPen(Qt::white);
        QFont badgeFont = painter.font();
        badgeFont.setBold(true);
        badgeFont.setPixelSize(12);
        painter.setFont(badgeFont);
        const auto badge = unreadCount > 9 ? QStringLiteral("9+")
                                           : QString::number(unreadCount);
        painter.drawText(QRectF(42, 4, 18, 18), Qt::AlignCenter, badge);
      }
    }

    painter.end();
    icon.addPixmap(pixmap);
  }
  return icon;
}
