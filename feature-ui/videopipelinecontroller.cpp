#include "videopipelinecontroller.h"

#include "hdmirxcontroller.h"

#include <QMetaObject>

#include <gst/video/video.h>

namespace {

bool hasProperty(GObject *object, const char *propertyName)
{
    return object && g_object_class_find_property(
                G_OBJECT_GET_CLASS(object), propertyName);
}

void setBooleanIfPresent(GObject *object, const char *propertyName, gboolean value)
{
    if (hasProperty(object, propertyName))
        g_object_set(object, propertyName, value, nullptr);
}

void setEnumIfPresent(GObject *object, const char *propertyName, gint value)
{
    if (hasProperty(object, propertyName))
        g_object_set(object, propertyName, value, nullptr);
}

} // namespace

VideoPipelineController::VideoPipelineController(HdmiRxController *hdmi,
                                                 QObject *parent)
    : QObject(parent), m_hdmi(hdmi)
{
    gst_init(nullptr, nullptr);

    m_busTimer.setInterval(50);
    connect(&m_busTimer, &QTimer::timeout,
            this, &VideoPipelineController::processBusMessages);

    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(1000);
    connect(&m_retryTimer, &QTimer::timeout,
            this, &VideoPipelineController::retryCurrentFormat);

    connect(m_hdmi, &HdmiRxController::formatChanged,
            this, &VideoPipelineController::startForFormat);
    connect(m_hdmi, &HdmiRxController::signalLost,
            this, &VideoPipelineController::handleSignalLost);
}

VideoPipelineController::~VideoPipelineController()
{
    stop();
}

QString VideoPipelineController::stateText() const
{
    switch (m_state) {
    case Stopped: return tr("等待HDMI信号");
    case Starting: return tr("正在启动视频");
    case Streaming: return tr("视频运行中");
    case Reconfiguring: return tr("正在切换视频格式");
    case PipelineError: return tr("视频管线错误");
    }
    return tr("未知状态");
}

QString VideoPipelineController::fillModeText() const
{
    return m_fillMode == Fit ? tr("适应") : tr("填满");
}

void VideoPipelineController::startForFormat(const VideoFormat &format)
{
    if (!format.isValid())
        return;

    m_retryTimer.stop();
    if (m_pipeline)
        setState(Reconfiguring);
    teardownPipeline();

    m_currentFormat = format;
    m_droppedFrames = 0;
    emit droppedFramesChanged();
    setState(Starting);
    setLastError(QString());

    QString caps = QStringLiteral(
                "video/x-raw,format=%1,width=%2,height=%3,framerate=%4/%5")
            .arg(format.gstFormat)
            .arg(format.width)
            .arg(format.height)
            .arg(format.fpsNumerator)
            .arg(format.fpsDenominator);
    if (format.colorimetry != QStringLiteral("unknown")
            && !format.colorimetry.isEmpty()) {
        caps += QStringLiteral(",colorimetry=%1").arg(format.colorimetry);
    }

    const QString description = QStringLiteral(
                "v4l2src name=hdmiSource device=/dev/video80 io-mode=mmap "
                "do-timestamp=true ! %1 ! "
                "queue name=latestQueue max-size-buffers=1 "
                "max-size-bytes=0 max-size-time=0 leaky=downstream ! "
                "waylandsink name=videoSink")
            .arg(caps);

    GError *parseError = nullptr;
    m_pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!m_pipeline) {
        const QString message = parseError
                ? QString::fromUtf8(parseError->message)
                : tr("无法创建GStreamer管线");
        if (parseError)
            g_error_free(parseError);
        handlePipelineFailure(message);
        return;
    }

    m_sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "videoSink");
    m_queue = gst_bin_get_by_name(GST_BIN(m_pipeline), "latestQueue");
    m_bus = gst_element_get_bus(m_pipeline);

    if (!m_sink || !m_queue || !m_bus) {
        handlePipelineFailure(tr("GStreamer管线缺少必要组件"));
        return;
    }

    applySinkProperties();
    g_signal_connect(m_queue, "overrun",
                     G_CALLBACK(VideoPipelineController::queueOverrunCallback),
                     this);

    const GstStateChangeReturn result = gst_element_set_state(
                m_pipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE) {
        handlePipelineFailure(tr("GStreamer无法进入PLAYING状态"));
        return;
    }

    m_busTimer.start();
}

void VideoPipelineController::handleSignalLost()
{
    m_retryTimer.stop();
    teardownPipeline();
    m_currentFormat = VideoFormat();
    setLastError(QString());
    setState(Stopped);
}

void VideoPipelineController::stop()
{
    m_retryTimer.stop();
    teardownPipeline();
    m_currentFormat = VideoFormat();
    setState(Stopped);
}

void VideoPipelineController::setFillMode(VideoPipelineController::FillMode mode)
{
    if (m_fillMode == mode)
        return;

    m_fillMode = mode;
    applySinkProperties();
    emit fillModeChanged();
}

void VideoPipelineController::toggleFillMode()
{
    setFillMode(m_fillMode == Fit ? Fill : Fit);
}

void VideoPipelineController::processBusMessages()
{
    if (!m_bus)
        return;

    for (;;) {
        GstMessage *message = gst_bus_pop(m_bus);
        if (!message)
            break;

        bool pipelineFailed = false;
        QString failureText;

        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: {
            GError *error = nullptr;
            gchar *debugText = nullptr;
            gst_message_parse_error(message, &error, &debugText);
            failureText = error
                    ? QString::fromUtf8(error->message)
                    : tr("未知GStreamer错误");
            if (debugText && *debugText)
                failureText += QStringLiteral(" | ") + QString::fromUtf8(debugText);
            if (error)
                g_error_free(error);
            g_free(debugText);
            pipelineFailed = true;
            break;
        }
        case GST_MESSAGE_EOS:
            failureText = tr("视频管线意外结束");
            pipelineFailed = true;
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
                GstState oldState;
                GstState newState;
                GstState pendingState;
                gst_message_parse_state_changed(message, &oldState,
                                                &newState, &pendingState);
                Q_UNUSED(oldState)
                Q_UNUSED(pendingState)
                if (newState == GST_STATE_PLAYING)
                    setState(Streaming);
            }
            break;
        default:
            break;
        }

        gst_message_unref(message);

        if (pipelineFailed) {
            handlePipelineFailure(failureText);
            break;
        }
    }
}

void VideoPipelineController::retryCurrentFormat()
{
    if (m_hdmi && m_hdmi->signalPresent() && m_currentFormat.isValid())
        startForFormat(m_currentFormat);
}

void VideoPipelineController::incrementDroppedFrames()
{
    ++m_droppedFrames;
    emit droppedFramesChanged();
}

void VideoPipelineController::queueOverrunCallback(GstElement *, gpointer userData)
{
    auto *controller = static_cast<VideoPipelineController *>(userData);
    QMetaObject::invokeMethod(controller, "incrementDroppedFrames",
                              Qt::QueuedConnection);
}

void VideoPipelineController::teardownPipeline()
{
    m_busTimer.stop();

    if (m_pipeline)
        gst_element_set_state(m_pipeline, GST_STATE_NULL);

    if (m_bus) {
        gst_object_unref(m_bus);
        m_bus = nullptr;
    }
    if (m_queue) {
        gst_object_unref(m_queue);
        m_queue = nullptr;
    }
    if (m_sink) {
        gst_object_unref(m_sink);
        m_sink = nullptr;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}

void VideoPipelineController::handlePipelineFailure(const QString &errorText)
{
    teardownPipeline();
    setLastError(errorText);
    setState(PipelineError);

    if (m_hdmi && m_hdmi->signalPresent() && m_currentFormat.isValid())
        m_retryTimer.start();
}

void VideoPipelineController::applySinkProperties()
{
    if (!m_sink)
        return;

    GObject *sinkObject = G_OBJECT(m_sink);
    setBooleanIfPresent(sinkObject, "sync", FALSE);
    setBooleanIfPresent(sinkObject, "fullscreen", TRUE);

    // Keep video above the desktop; the QML window stays on top for the HUD.
    setEnumIfPresent(sinkObject, "layer", 1);
    setEnumIfPresent(sinkObject, "fill-mode", m_fillMode == Fit ? 1 : 2);
    setEnumIfPresent(sinkObject, "rotate-method", GST_VIDEO_ORIENTATION_90R);
}

void VideoPipelineController::setState(VideoPipelineController::State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void VideoPipelineController::setLastError(const QString &errorText)
{
    if (m_lastError == errorText)
        return;
    m_lastError = errorText;
    emit lastErrorChanged();
}
