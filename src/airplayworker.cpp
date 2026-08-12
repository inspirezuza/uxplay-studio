#include "airplayworker.h"

#include "uxplay_api.h"

#include <QByteArray>
#include <QDebug>
#include <QImage>
#include <vector>

AirPlayWorker::AirPlayWorker(QObject *parent) : QThread(parent) {
    qRegisterMetaType<ReceiverEvent>();
}

void AirPlayWorker::configure(QStringList args, quintptr videoWindow) {
    Q_ASSERT(!isRunning());
    m_args = std::move(args);
    m_videoWindow = videoWindow;
}

void AirPlayWorker::run() {
    std::vector<QByteArray> encodedArguments;
    encodedArguments.reserve(static_cast<size_t>(m_args.size() + 1));
    encodedArguments.emplace_back("uxplay-studio");
    for (const QString &argument : m_args) {
        if (!argument.trimmed().isEmpty()) {
            encodedArguments.push_back(argument.toUtf8());
        }
    }

    // Build the pointer vector only after QByteArray storage is complete so a
    // vector reallocation can never invalidate argv.
    std::vector<char *> argv;
    argv.reserve(encodedArguments.size());
    for (QByteArray &argument : encodedArguments) {
        argv.push_back(argument.data());
    }

    uxplay_set_video_window(static_cast<uintptr_t>(m_videoWindow));
    uxplay_set_event_callback(&AirPlayWorker::eventCallback, this);
    uxplay_set_preview_callback(&AirPlayWorker::previewCallback, this);
    emit engineStarted();
    const int exitCode = start_uxplay(static_cast<int>(argv.size()), argv.data());
    uxplay_set_event_callback(nullptr, nullptr);
    uxplay_set_preview_callback(nullptr, nullptr);
    emit engineExited(exitCode);
}

void AirPlayWorker::stopAirplay() {
    requestInterruption();
    stop_uxplay();
}

void AirPlayWorker::eventCallback(const uxplay_event *event, void *context) {
    if (!event || !context) {
        return;
    }
    auto *worker = static_cast<AirPlayWorker *>(context);
    ReceiverEvent translated;
    translated.type = static_cast<int>(event->type);
    translated.deviceName = QString::fromUtf8(event->device_name ? event->device_name : "");
    translated.deviceModel = QString::fromUtf8(event->device_model ? event->device_model : "");
    translated.deviceId = QString::fromUtf8(event->device_id ? event->device_id : "");
    translated.message = QString::fromUtf8(event->message ? event->message : "");
    translated.width = event->width;
    translated.height = event->height;
    emit worker->receiverEvent(translated);
}

void AirPlayWorker::previewCallback(const unsigned char *data, int width, int height,
                                    int stride, void *context) {
    if (!data || width <= 0 || height <= 0 || stride <= 0 || !context) return;
    auto *worker = static_cast<AirPlayWorker *>(context);
    const QImage frame(data, width, height, stride, QImage::Format_ARGB32);
    emit worker->previewFrame(frame.copy());
}
