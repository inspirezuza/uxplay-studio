#pragma once

#include <QImage>
#include <QWidget>

class QContextMenuEvent;

class CameraSelfView final : public QWidget {
    Q_OBJECT

public:
    explicit CameraSelfView(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setActive(bool active);
    void setOverlayVisible(bool visible);
    bool isActive() const;

signals:
    void contextMenuRequested(const QPoint &globalPosition);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void clampToParent();

    QImage m_frame;
    QPoint m_pressGlobal;
    QRect m_pressGeometry;
    bool m_active = false;
    bool m_overlayVisible = true;
    bool m_resizing = false;
};
