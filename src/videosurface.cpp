#include "videosurface.h"

#include <QLabel>
#include <QPainter>
#include <QStackedLayout>
#include <QVBoxLayout>

class NativeRenderTarget final : public QWidget {
public:
    explicit NativeRenderTarget(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setAutoFillBackground(false);
        setObjectName(QStringLiteral("nativeVideoTarget"));
    }

    void setStreaming(bool streaming) {
        m_streaming = streaming;
        if (!streaming) {
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (m_streaming) {
            return;
        }
        QPainter painter(this);
        painter.fillRect(rect(), QColor(QStringLiteral("#050914")));
    }

private:
    bool m_streaming = false;
};

VideoSurface::VideoSurface(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("videoSurface"));
    setMinimumSize(560, 315);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);

    m_renderTarget = new NativeRenderTarget(this);
    m_stack->addWidget(m_renderTarget);

    m_placeholder = new QWidget(this);
    m_placeholder->setObjectName(QStringLiteral("videoPlaceholder"));
    auto *layout = new QVBoxLayout(m_placeholder);
    layout->setContentsMargins(48, 48, 48, 48);
    layout->addStretch();

    auto *mark = new QLabel(QStringLiteral("◉"), m_placeholder);
    mark->setObjectName(QStringLiteral("airplayMark"));
    mark->setAlignment(Qt::AlignCenter);
    layout->addWidget(mark);

    m_placeholderTitle = new QLabel(QStringLiteral("Starting the receiver…"), m_placeholder);
    m_placeholderTitle->setObjectName(QStringLiteral("videoPlaceholderTitle"));
    m_placeholderTitle->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_placeholderTitle);

    m_placeholderDetail = new QLabel(
        QStringLiteral("Keep your iPad and this PC on the same Wi-Fi network."), m_placeholder);
    m_placeholderDetail->setObjectName(QStringLiteral("videoPlaceholderDetail"));
    m_placeholderDetail->setAlignment(Qt::AlignCenter);
    m_placeholderDetail->setWordWrap(true);
    layout->addWidget(m_placeholderDetail);
    layout->addStretch();
    m_stack->addWidget(m_placeholder);
    m_stack->setCurrentWidget(m_placeholder);

    // Force creation now. The handle remains stable for the entire receiver
    // session and is passed directly to GstVideoOverlay.
    m_renderTarget->winId();
}

quintptr VideoSurface::nativeHandle() const {
    return static_cast<quintptr>(m_renderTarget->winId());
}

bool VideoSurface::isStreaming() const {
    return m_streaming;
}

void VideoSurface::setStreaming(bool streaming) {
    if (m_streaming == streaming) {
        return;
    }
    m_streaming = streaming;
    m_renderTarget->setStreaming(streaming);
    m_stack->setCurrentWidget(streaming ? static_cast<QWidget *>(m_renderTarget)
                                        : m_placeholder);
    if (!streaming) {
        m_renderTarget->update();
    }
}

void VideoSurface::setPlaceholderText(const QString &title, const QString &detail) {
    m_placeholderTitle->setText(title);
    m_placeholderDetail->setText(detail);
}
