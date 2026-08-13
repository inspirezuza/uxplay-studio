#include "studiovisuals.h"

#include <QColor>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace StudioVisuals {

QIcon icon(const QString &name) {
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(QStringLiteral("#B7BBC6")), 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (name == QStringLiteral("studio")) {
        painter.drawRoundedRect(QRectF(3, 4, 18, 14), 2, 2);
        painter.drawLine(QPointF(8, 21), QPointF(16, 21));
        painter.drawLine(QPointF(12, 18), QPointF(12, 21));
    } else if (name == QStringLiteral("projects")) {
        QPainterPath path;
        path.moveTo(3, 7); path.lineTo(10, 7); path.lineTo(12, 9);
        path.lineTo(21, 9); path.lineTo(21, 19); path.lineTo(3, 19); path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(QPointF(3, 7), QPointF(3, 5));
        painter.drawLine(QPointF(3, 5), QPointF(10, 5));
        painter.drawLine(QPointF(10, 5), QPointF(12, 7));
    } else if (name == QStringLiteral("settings")) {
        painter.drawEllipse(QPointF(12, 12), 3, 3);
        painter.drawEllipse(QPointF(12, 12), 8, 8);
        constexpr qreal Pi = 3.14159265358979323846;
        for (int i = 0; i < 8; ++i) {
            const qreal angle = i * Pi / 4.0;
            painter.drawLine(QPointF(12 + std::cos(angle) * 8, 12 + std::sin(angle) * 8),
                             QPointF(12 + std::cos(angle) * 10, 12 + std::sin(angle) * 10));
        }
    } else if (name == QStringLiteral("airplay")) {
        painter.drawRoundedRect(QRectF(3, 3, 18, 14), 2, 2);
        painter.drawLine(QPointF(7, 21), QPointF(12, 16));
        painter.drawLine(QPointF(12, 16), QPointF(17, 21));
    } else if (name == QStringLiteral("camera")) {
        painter.drawRoundedRect(QRectF(3, 6, 18, 13), 2, 2);
        painter.drawEllipse(QPointF(12, 12.5), 4, 4);
        painter.drawLine(QPointF(9, 6), QPointF(10.5, 4));
        painter.drawLine(QPointF(10.5, 4), QPointF(14, 4));
        painter.drawLine(QPointF(14, 4), QPointF(15, 6));
    } else if (name == QStringLiteral("image")) {
        painter.drawRoundedRect(QRectF(3, 4, 18, 16), 2, 2);
        painter.drawEllipse(QPointF(8, 9), 2, 2);
        painter.drawLine(QPointF(4, 17), QPointF(9, 12));
        painter.drawLine(QPointF(9, 12), QPointF(13, 16));
        painter.drawLine(QPointF(13, 16), QPointF(16, 13));
        painter.drawLine(QPointF(16, 13), QPointF(20, 18));
    } else if (name == QStringLiteral("text")) {
        painter.drawLine(QPointF(4, 5), QPointF(20, 5));
        painter.drawLine(QPointF(12, 5), QPointF(12, 20));
        painter.drawLine(QPointF(8, 20), QPointF(16, 20));
    } else if (name == QStringLiteral("color")) {
        QPainterPath drop;
        drop.moveTo(12, 3); drop.cubicTo(8, 8, 6, 11, 6, 15);
        drop.cubicTo(6, 19, 9, 21, 12, 21); drop.cubicTo(15, 21, 18, 19, 18, 15);
        drop.cubicTo(18, 11, 16, 8, 12, 3); drop.closeSubpath();
        painter.drawPath(drop);
    } else if (name == QStringLiteral("fullscreen")) {
        painter.drawLine(QPointF(8, 3), QPointF(3, 3)); painter.drawLine(QPointF(3, 3), QPointF(3, 8));
        painter.drawLine(QPointF(16, 3), QPointF(21, 3)); painter.drawLine(QPointF(21, 3), QPointF(21, 8));
        painter.drawLine(QPointF(8, 21), QPointF(3, 21)); painter.drawLine(QPointF(3, 21), QPointF(3, 16));
        painter.drawLine(QPointF(16, 21), QPointF(21, 21)); painter.drawLine(QPointF(21, 21), QPointF(21, 16));
    } else if (name == QStringLiteral("export")) {
        painter.drawLine(QPointF(12, 3), QPointF(12, 15)); painter.drawLine(QPointF(12, 3), QPointF(8, 7));
        painter.drawLine(QPointF(12, 3), QPointF(16, 7)); painter.drawLine(QPointF(5, 13), QPointF(5, 20));
        painter.drawLine(QPointF(5, 20), QPointF(19, 20)); painter.drawLine(QPointF(19, 20), QPointF(19, 13));
    } else if (name == QStringLiteral("up")) {
        painter.drawLine(QPointF(6, 15), QPointF(12, 9)); painter.drawLine(QPointF(12, 9), QPointF(18, 15));
    } else if (name == QStringLiteral("down")) {
        painter.drawLine(QPointF(6, 9), QPointF(12, 15)); painter.drawLine(QPointF(12, 15), QPointF(18, 9));
    } else if (name == QStringLiteral("lock")) {
        painter.drawRoundedRect(QRectF(4, 10, 16, 11), 2, 2);
        painter.drawArc(QRectF(8, 3, 8, 10), 0, 180 * 16);
    } else if (name == QStringLiteral("trash")) {
        painter.drawLine(QPointF(4, 7), QPointF(20, 7)); painter.drawRect(QRectF(7, 7, 10, 14));
        painter.drawLine(QPointF(9, 3), QPointF(15, 3)); painter.drawLine(QPointF(10, 3), QPointF(9, 7));
        painter.drawLine(QPointF(14, 3), QPointF(15, 7));
    }
    return QIcon(pixmap);
}

QPixmap projectThumbnail(const QString &title, bool recoverable) {
    QPixmap thumbnail(480, 270);
    thumbnail.fill(QColor(QStringLiteral("#0E1014")));
    QPainter painter(&thumbnail);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, thumbnail.width(), thumbnail.height());
    gradient.setColorAt(0, QColor(QStringLiteral("#192038")));
    gradient.setColorAt(1, QColor(QStringLiteral("#111318")));
    painter.fillRect(thumbnail.rect(), gradient);
    painter.setPen(QPen(QColor(QStringLiteral("#5B8CFF")), 4));
    painter.drawRoundedRect(QRectF(158, 54, 164, 112), 14, 14);
    painter.drawLine(QPointF(204, 208), QPointF(240, 174));
    painter.drawLine(QPointF(240, 174), QPointF(276, 208));
    painter.setPen(QColor(QStringLiteral("#E8EAF0")));
    QFont font = painter.font();
    font.setPixelSize(22);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.drawText(QRect(24, 212, 432, 40), Qt::AlignCenter, title.left(34));
    if (recoverable) {
        painter.setBrush(QColor(QStringLiteral("#E8BB58")));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(440, 28), 8, 8);
    }
    return thumbnail;
}

} // namespace StudioVisuals
