#include "cameraselfview.h"

#include <QEvent>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

CameraSelfView::CameraSelfView(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("cameraSelfView"));
    setAccessibleName(QStringLiteral("cameraSelfView"));
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMinimumSize(144, 81);
    resize(240, 135);
    setCursor(Qt::SizeAllCursor);
    if (parent) parent->installEventFilter(this);
    hide();
}

void CameraSelfView::setFrame(const QImage &frame) {
    if (frame.isNull()) return;
    m_frame = frame;
    if (m_active && m_overlayVisible) {
        clampToParent();
        show();
        raise();
    }
    update();
}

void CameraSelfView::setActive(bool active) {
    m_active = active;
    setVisible(active && m_overlayVisible && !m_frame.isNull());
    if (isVisible()) {
        clampToParent();
        raise();
    }
}

bool CameraSelfView::isActive() const {
    return m_active;
}

void CameraSelfView::setOverlayVisible(bool visible) {
    m_overlayVisible = visible;
    setVisible(m_active && m_overlayVisible && !m_frame.isNull());
    if (isVisible()) {
        clampToParent();
        raise();
    }
}

bool CameraSelfView::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) clampToParent();
    return QWidget::eventFilter(watched, event);
}

void CameraSelfView::contextMenuEvent(QContextMenuEvent *event) {
    emit contextMenuRequested(event->globalPos());
    event->accept();
}

void CameraSelfView::paintEvent(QPaintEvent *) {
    if (m_frame.isNull()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF target = rect().adjusted(3, 3, -3, -3);
    QPainterPath clip;
    clip.addRoundedRect(target, 16, 16);
    painter.setClipPath(clip);
    const qreal sourceAspect = static_cast<qreal>(m_frame.width()) / m_frame.height();
    const qreal targetAspect = target.width() / target.height();
    QRectF source(m_frame.rect());
    if (sourceAspect > targetAspect) {
        const qreal width = m_frame.height() * targetAspect;
        source.setLeft((m_frame.width() - width) / 2.0);
        source.setWidth(width);
    } else if (sourceAspect < targetAspect) {
        const qreal height = m_frame.width() / targetAspect;
        source.setTop((m_frame.height() - height) / 2.0);
        source.setHeight(height);
    }
    painter.drawImage(target, m_frame, source);
    painter.setClipping(false);
    painter.setPen(QPen(QColor(QStringLiteral("#73a0ff")), 3));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(target, 16, 16);
    painter.setBrush(QColor(QStringLiteral("#73a0ff")));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(width() - 12, height() - 12), 4, 4);
}

void CameraSelfView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    m_pressGlobal = event->globalPosition().toPoint();
    m_pressGeometry = geometry();
    m_resizing = event->position().x() >= width() - 24 &&
                 event->position().y() >= height() - 24;
    setCursor(m_resizing ? Qt::SizeFDiagCursor : Qt::ClosedHandCursor);
    event->accept();
}

void CameraSelfView::mouseMoveEvent(QMouseEvent *event) {
    if (!(event->buttons() & Qt::LeftButton)) return;
    const QPoint delta = event->globalPosition().toPoint() - m_pressGlobal;
    if (m_resizing) {
        QSize next = m_pressGeometry.size() + QSize(delta.x(), delta.y());
        next = next.expandedTo(minimumSize());
        next.setWidth(qMin(next.width(), parentWidget()->width()));
        next.setHeight(qMax(minimumHeight(), qRound(next.width() * 9.0 / 16.0)));
        resize(next);
    } else {
        move(m_pressGeometry.topLeft() + delta);
    }
    clampToParent();
    event->accept();
}

void CameraSelfView::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        setCursor(Qt::SizeAllCursor);
        m_resizing = false;
        event->accept();
    }
}

void CameraSelfView::clampToParent() {
    if (!parentWidget()) return;
    const int margin = 16;
    if (x() == 0 && y() == 0)
        move(qMax(margin, parentWidget()->width() - width() - margin),
             qMax(margin, parentWidget()->height() - height() - margin));
    move(qBound(margin, x(), qMax(margin, parentWidget()->width() - width() - margin)),
         qBound(margin, y(), qMax(margin, parentWidget()->height() - height() - margin)));
}
