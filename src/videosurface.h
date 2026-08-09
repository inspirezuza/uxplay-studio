#pragma once

#include <QWidget>

class QLabel;
class NativeRenderTarget;
class QStackedLayout;

class VideoSurface final : public QWidget {
    Q_OBJECT

public:
    explicit VideoSurface(QWidget *parent = nullptr);

    quintptr nativeHandle() const;
    bool isStreaming() const;
    void setStreaming(bool streaming);
    void setPlaceholderText(const QString &title, const QString &detail);

private:
    QStackedLayout *m_stack = nullptr;
    NativeRenderTarget *m_renderTarget = nullptr;
    QWidget *m_placeholder = nullptr;
    QLabel *m_placeholderTitle = nullptr;
    QLabel *m_placeholderDetail = nullptr;
    bool m_streaming = false;
};
