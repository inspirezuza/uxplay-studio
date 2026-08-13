#pragma once

#include <QImage>
#include <QWidget>

class CameraSelfView final : public QWidget {
    Q_OBJECT

public:
    explicit CameraSelfView(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setActive(bool active);
    bool isActive() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
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
    bool m_resizing = false;
};
