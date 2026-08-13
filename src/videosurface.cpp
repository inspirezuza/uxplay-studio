#include "videosurface.h"

#include <QLabel>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QStackedLayout>
#include <QVBoxLayout>

namespace {
QPixmap receiverIcon() {
    QPixmap pixmap(66, 66);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#83A9FF")), 2.2,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(QColor(QStringLiteral("#172039")));
    painter.drawEllipse(QRectF(1, 1, 64, 64));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(17, 17, 32, 23), 3, 3);
    painter.drawLine(QPointF(24, 49), QPointF(33, 40));
    painter.drawLine(QPointF(33, 40), QPointF(42, 49));
    return pixmap;
}
}

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
    setMinimumSize(480, 270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);

    m_renderTarget = new NativeRenderTarget(this);
    m_stack->addWidget(m_renderTarget);

    m_placeholder = new QWidget(this);
    m_placeholder->setObjectName(QStringLiteral("videoPlaceholder"));
    auto *layout = new QVBoxLayout(m_placeholder);
    layout->setContentsMargins(42, 32, 42, 34);
    layout->setSpacing(8);
    layout->addStretch();

    auto *mark = new QLabel(QStringLiteral("◉"), m_placeholder);
    mark->setText(QString());
    mark->setObjectName(QStringLiteral("airplayMark"));
    mark->setAlignment(Qt::AlignCenter);
    mark->setPixmap(receiverIcon());
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
    layout->addSpacing(22);

    auto *steps = new QHBoxLayout;
    steps->setSpacing(8);
    const QList<QPair<QString, QString>> copy {
        {QStringLiteral("Same network"), QStringLiteral("Keep this PC and iPad on the same Wi-Fi.")},
        {QStringLiteral("Screen Mirroring"), QStringLiteral("Open Control Center on your iPad.")},
        {QStringLiteral("Choose this PC"), QStringLiteral("Select the receiver name shown above.")}
    };
    int index = 1;
    for (const auto &step : copy) {
        auto *stepCard = new QWidget(m_placeholder);
        stepCard->setObjectName(QStringLiteral("connectStep"));
        auto *stepLayout = new QVBoxLayout(stepCard);
        stepLayout->setContentsMargins(14, 12, 14, 12);
        stepLayout->setSpacing(4);
        auto *number = new QLabel(QString::number(index++), stepCard);
        number->setObjectName(QStringLiteral("stepNumber"));
        stepLayout->addWidget(number);
        auto *title = new QLabel(step.first, stepCard);
        title->setObjectName(QStringLiteral("stepTitle"));
        stepLayout->addWidget(title);
        auto *detail = new QLabel(step.second, stepCard);
        detail->setObjectName(QStringLiteral("stepCopy"));
        detail->setWordWrap(true);
        stepLayout->addWidget(detail);
        steps->addWidget(stepCard, 1);
    }
    layout->addLayout(steps);
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
