#include "lowlatencyvideopipelinecontroller.h"

#include "hdmirxcontroller.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace {

constexpr char kVideoDevice[] = "/dev/video80";
constexpr char kBaselineEnvironment[] = "MONITOR_DEMO_LOW_LATENCY";
constexpr char kReadyMarker[] = "STREAM_READY";

} // namespace

LowLatencyVideoPipelineController::LowLatencyVideoPipelineController(
        HdmiRxController *hdmi, QObject *parent)
    : QObject(parent), m_hdmi(hdmi)
{
    m_useExplicitSync = qgetenv(kBaselineEnvironment) != QByteArrayLiteral("0");
    m_process.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&m_process, &QProcess::started,
            this, &LowLatencyVideoPipelineController::handleProcessStarted);
    connect(&m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &LowLatencyVideoPipelineController::handleProcessFinished);
    connect(&m_process, &QProcess::errorOccurred,
            this, &LowLatencyVideoPipelineController::handleProcessError);
    connect(&m_process, &QProcess::readyReadStandardOutput,
            this, &LowLatencyVideoPipelineController::drainStandardOutput);
    connect(&m_process, &QProcess::readyReadStandardError,
            this, &LowLatencyVideoPipelineController::drainStandardError);

    m_retryTimer.setSingleShot(true);
    m_retryTimer.setInterval(1000);
    connect(&m_retryTimer, &QTimer::timeout,
            this, &LowLatencyVideoPipelineController::retryCurrentFormat);

    connect(m_hdmi, &HdmiRxController::formatChanged,
            this, &LowLatencyVideoPipelineController::startForFormat);
    connect(m_hdmi, &HdmiRxController::signalLost,
            this, &LowLatencyVideoPipelineController::handleSignalLost);
}

LowLatencyVideoPipelineController::~LowLatencyVideoPipelineController()
{
    stop();
}

QString LowLatencyVideoPipelineController::stateText() const
{
    switch (m_state) {
    case Stopped: return tr("等待HDMI信号");
    case Starting: return tr("正在启动低延迟视频");
    case Streaming: return tr("低延迟视频运行中");
    case Reconfiguring: return tr("正在切换视频格式");
    case PipelineError: return tr("低延迟视频错误");
    }
    return tr("未知状态");
}

QString LowLatencyVideoPipelineController::fillModeText() const
{
    return m_fillMode == Fit ? tr("适应") : tr("填满");
}

void LowLatencyVideoPipelineController::startForFormat(const VideoFormat &format)
{
    if (!format.isValid())
        return;

    m_retryTimer.stop();
    if (m_phase != Idle)
        setState(Reconfiguring);
    stopProcessAndRestore();

    m_currentFormat = format;
    if (format.driverFormat != QStringLiteral("NV16")
            && format.driverFormat != QStringLiteral("NV24")) {
        m_currentFormat = VideoFormat();
        handleFailure(tr("低延迟通路暂不支持格式 %1")
                      .arg(format.driverFormat));
        return;
    }

    // DSI-2 product path: Esmart3 has no hw scaling, so helper uses
    // wp_viewport_set_source to crop the 4K buffer to output size.
    // NV16/NV24 at 1080x1920 src rect fits Esmart3 without scaling.
    if (m_falseColorEnabled && format.driverFormat != QStringLiteral("NV16")) {
        m_falseColorEnabled = false;
        emit imageAssistChanged();
    }
    if (m_zoomFactor != 1 && format.driverFormat != QStringLiteral("NV16")) {
        m_zoomFactor = 1;
        emit zoomFactorChanged();
    }
    if (format.driverFormat != QStringLiteral("NV16")
            && (m_zebraEnabled || m_peakingEnabled || m_waveformEnabled)) {
        m_zebraEnabled = false;
        m_peakingEnabled = false;
        m_waveformEnabled = false;
        emit imageAssistChanged();
        clearAnalysisFrame();
    }

    m_v4l2Format = format.driverFormat;
    m_drmFormat = m_falseColorEnabled ? QStringLiteral("XB24")
                                      : format.driverFormat;

    setLastError(QString());
    setState(Starting);
    startProbe();
}

void LowLatencyVideoPipelineController::handleSignalLost()
{
    m_retryTimer.stop();
    stopProcessAndRestore();
    m_currentFormat = VideoFormat();
    clearAnalysisFrame();
    setLastError(QString());
    setState(Stopped);
}

void LowLatencyVideoPipelineController::stop()
{
    m_retryTimer.stop();
    stopProcessAndRestore();
    m_currentFormat = VideoFormat();
    clearAnalysisFrame();
    setState(Stopped);
}

void LowLatencyVideoPipelineController::setFillMode(FillMode mode)
{
    if (m_fillMode == mode)
        return;
    m_fillMode = mode;
    emit fillModeChanged();
}

void LowLatencyVideoPipelineController::toggleFillMode()
{
    setFillMode(m_fillMode == Fit ? Fill : Fit);
}

void LowLatencyVideoPipelineController::setFalseColorEnabled(bool enabled)
{
    if (m_falseColorEnabled == enabled)
        return;
    m_falseColorEnabled = enabled;
    emit imageAssistChanged();
    restartForSettings();
}

void LowLatencyVideoPipelineController::toggleFalseColor()
{
    setFalseColorEnabled(!m_falseColorEnabled);
}

void LowLatencyVideoPipelineController::setZoomFactor(int factor)
{
    if (factor != 1 && factor != 2 && factor != 4)
        return;
    if (m_zoomFactor == factor)
        return;
    m_zoomFactor = factor;
    emit zoomFactorChanged();
    restartForSettings();
}

void LowLatencyVideoPipelineController::cycleZoomFactor()
{
    setZoomFactor(m_zoomFactor == 1 ? 2 : m_zoomFactor == 2 ? 4 : 1);
}

void LowLatencyVideoPipelineController::setZebraEnabled(bool enabled)
{
    if (m_zebraEnabled == enabled)
        return;
    m_zebraEnabled = enabled;
    emit imageAssistChanged();
    if (!enabled)
        clearAnalysisFrame();
    restartForSettings();
}

void LowLatencyVideoPipelineController::setZebraLevel(int level)
{
    level = qBound(0, level, 100);
    if (m_zebraLevel == level)
        return;
    m_zebraLevel = level;
    emit imageAssistChanged();
    if (m_zebraEnabled)
        restartForSettings();
}

void LowLatencyVideoPipelineController::setPeakingEnabled(bool enabled)
{
    if (m_peakingEnabled == enabled)
        return;
    m_peakingEnabled = enabled;
    emit imageAssistChanged();
    if (!enabled)
        clearAnalysisFrame();
    restartForSettings();
}

void LowLatencyVideoPipelineController::setPeakingSensitivity(int sensitivity)
{
    sensitivity = qBound(1, sensitivity, 100);
    if (m_peakingSensitivity == sensitivity)
        return;
    m_peakingSensitivity = sensitivity;
    emit imageAssistChanged();
    if (m_peakingEnabled)
        restartForSettings();
}

void LowLatencyVideoPipelineController::setWaveformEnabled(bool enabled)
{
    if (m_waveformEnabled == enabled)
        return;
    m_waveformEnabled = enabled;
    emit imageAssistChanged();
    if (!enabled)
        clearAnalysisFrame();
    restartForSettings();
}

QString LowLatencyVideoPipelineController::helperPath() const
{
    return QCoreApplication::applicationDirPath()
            + QStringLiteral("/v4l2_wayland_explicit_sync");
}

void LowLatencyVideoPipelineController::startProbe()
{
    const QString program = helperPath();
    const QFileInfo helper(program);
    if (!helper.isFile() || !helper.isExecutable()) {
        handleFailure(tr("低延迟 helper 不存在或不可执行: %1").arg(program));
        return;
    }

    m_processOutput.clear();
    m_stdoutBuffer.clear();
    m_phase = Probing;
    m_process.start(program,
                    {QStringLiteral("-p"),
                     QStringLiteral("-g"),
                     QStringLiteral("-v"), QString::fromLatin1(kVideoDevice),
                     QStringLiteral("-f"), m_v4l2Format,
                     QStringLiteral("-d"), m_drmFormat});
}

void LowLatencyVideoPipelineController::startHelper()
{
    m_hdmi->pauseDeviceMonitoring();
    if (!m_hdmi->setLowLatencyEnabled(m_useExplicitSync)) {
        m_hdmi->resumeDeviceMonitoring();
        handleFailure(tr("无法切换 HDMI RX 低延迟安全状态"));
        return;
    }

    QStringList arguments = {
        QStringLiteral("-v"), QString::fromLatin1(kVideoDevice),
        QStringLiteral("-f"), m_v4l2Format,
        QStringLiteral("-d"), m_drmFormat,
        QStringLiteral("-g"),
        QStringLiteral("-s")
    };
    if (m_zoomFactor != 1)
        arguments.append({QStringLiteral("-z"), QString::number(m_zoomFactor)});
    if (m_falseColorEnabled)
        arguments.append(QStringLiteral("-e"));
    if (m_zebraEnabled)
        arguments.append({QStringLiteral("-Z"), QString::number(m_zebraLevel)});
    if (m_peakingEnabled)
        arguments.append({QStringLiteral("-P"),
                          QString::number(m_peakingSensitivity)});
    if (m_waveformEnabled)
        arguments.append(QStringLiteral("-W"));
    if (!m_useExplicitSync)
        arguments.append(QStringLiteral("-q"));

    m_processOutput.clear();
    m_stdoutBuffer.clear();
    m_phase = Playing;
    m_process.start(helperPath(), arguments);
}

void LowLatencyVideoPipelineController::handleProcessStarted()
{
    if (m_phase == Playing)
        setState(Starting);
}

void LowLatencyVideoPipelineController::handleProcessFinished(
        int exitCode, QProcess::ExitStatus exitStatus)
{
    drainStandardOutput();
    drainStandardError();
    if (!m_stdoutBuffer.isEmpty()) {
        appendProcessText(m_stdoutBuffer);
        m_stdoutBuffer.clear();
    }
    if (m_stopping || m_phase == Idle)
        return;

    const ProcessPhase finishedPhase = m_phase;
    m_phase = Idle;
    if (finishedPhase == Probing) {
        if (exitStatus == QProcess::NormalExit && exitCode == 0) {
            startHelper();
            return;
        }
        handleFailure(outputErrorText(tr("低延迟能力探测失败")));
        return;
    }

    const bool restored = stopProcessAndRestore();
    const QString fallback = restored
            ? tr("低延迟视频进程意外退出")
            : tr("低延迟视频退出后未能恢复安全状态");
    handleFailure(outputErrorText(fallback));
}

void LowLatencyVideoPipelineController::handleProcessError(
        QProcess::ProcessError error)
{
    if (m_stopping || error != QProcess::FailedToStart)
        return;

    m_phase = Idle;
    stopProcessAndRestore();
    handleFailure(tr("无法启动低延迟 helper: %1")
                  .arg(m_process.errorString()));
}

void LowLatencyVideoPipelineController::drainStandardOutput()
{
    m_stdoutBuffer += m_process.readAllStandardOutput();
    parseAnalysisFrames();
}

void LowLatencyVideoPipelineController::drainStandardError()
{
    appendProcessText(m_process.readAllStandardError());
}

void LowLatencyVideoPipelineController::appendProcessText(const QByteArray &text)
{
    m_processOutput += text;
    if (m_phase == Playing && m_state != Streaming
            && m_processOutput.contains(kReadyMarker)) {
        setState(Streaming);
    }
    if (m_processOutput.size() > 8192)
        m_processOutput = m_processOutput.right(8192);
}

void LowLatencyVideoPipelineController::parseAnalysisFrames()
{
    static const QByteArray marker = QByteArrayLiteral("MDA1 ");
    QByteArray latestZebra;
    QByteArray latestPeaking;
    QByteArray latestWaveform;
    int latestWidth = 0;
    int latestHeight = 0;

    while (!m_stdoutBuffer.isEmpty()) {
        const int markerIndex = m_stdoutBuffer.indexOf(marker);
        if (markerIndex < 0) {
            const int newline = m_stdoutBuffer.lastIndexOf('\n');
            if (newline >= 0) {
                appendProcessText(m_stdoutBuffer.left(newline + 1));
                m_stdoutBuffer.remove(0, newline + 1);
            } else if (m_stdoutBuffer.size() > 65536) {
                appendProcessText(m_stdoutBuffer.left(
                                      m_stdoutBuffer.size() - marker.size()));
                m_stdoutBuffer = m_stdoutBuffer.right(marker.size());
            }
            break;
        }
        if (markerIndex > 0) {
            appendProcessText(m_stdoutBuffer.left(markerIndex));
            m_stdoutBuffer.remove(0, markerIndex);
        }

        const int newline = m_stdoutBuffer.indexOf('\n');
        if (newline < 0)
            break;

        const QList<QByteArray> fields = m_stdoutBuffer.left(newline).split(' ');
        bool widthOk = false;
        bool heightOk = false;
        bool bytesOk = false;
        const int width = fields.size() == 5 ? fields.at(2).toInt(&widthOk) : 0;
        const int height = fields.size() == 5 ? fields.at(3).toInt(&heightOk) : 0;
        const int bitmapBytes = fields.size() == 5
                ? fields.at(4).toInt(&bytesOk) : 0;
        const bool dimensionsOk = width > 0 && height > 0
                && width <= 512 && height <= 512;
        const int expectedBytes = dimensionsOk
                ? (width * height + 7) / 8 : 0;

        if (!widthOk || !heightOk || !bytesOk || !dimensionsOk
                || bitmapBytes != expectedBytes || bitmapBytes > 32768) {
            appendProcessText(m_stdoutBuffer.left(newline + 1));
            m_stdoutBuffer.remove(0, newline + 1);
            continue;
        }

        const int payloadStart = newline + 1;
        const int payloadSize = bitmapBytes * 3;
        if (m_stdoutBuffer.size() < payloadStart + payloadSize)
            break;

        latestZebra = m_stdoutBuffer.mid(payloadStart, bitmapBytes);
        latestPeaking = m_stdoutBuffer.mid(
                    payloadStart + bitmapBytes, bitmapBytes);
        latestWaveform = m_stdoutBuffer.mid(
                    payloadStart + bitmapBytes * 2, bitmapBytes);
        latestWidth = width;
        latestHeight = height;
        m_stdoutBuffer.remove(0, payloadStart + payloadSize);
    }

    if (latestWidth > 0)
        emit analysisFrameReady(latestWidth, latestHeight,
                                latestZebra, latestPeaking, latestWaveform);
}

void LowLatencyVideoPipelineController::clearAnalysisFrame()
{
    emit analysisFrameReady(0, 0, QByteArray(), QByteArray(), QByteArray());
}

bool LowLatencyVideoPipelineController::stopProcessAndRestore()
{
    m_stopping = true;
    bool safe = true;
    bool restoredBeforeKill = false;

    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(2000)) {
            safe = m_hdmi->setLowLatencyEnabled(false);
            restoredBeforeKill = true;
            m_process.kill();
            safe = m_process.waitForFinished(1000) && safe;
        }
    }

    if (!restoredBeforeKill)
        safe = m_hdmi->setLowLatencyEnabled(false) && safe;
    m_hdmi->resumeDeviceMonitoring();
    m_phase = Idle;
    m_stopping = false;
    return safe;
}

void LowLatencyVideoPipelineController::restartForSettings()
{
    if (!m_hdmi || !m_hdmi->signalPresent() || !m_currentFormat.isValid())
        return;
    clearAnalysisFrame();
    m_retryTimer.stop();
    startForFormat(m_currentFormat);
}

QString LowLatencyVideoPipelineController::outputErrorText(
        const QString &fallback) const
{
    const QString output = QString::fromLocal8Bit(m_processOutput).trimmed();
    return output.isEmpty() ? fallback
                            : fallback + QStringLiteral(": ") + output.right(2000);
}

void LowLatencyVideoPipelineController::handleFailure(const QString &errorText)
{
    setLastError(errorText);
    setState(PipelineError);
    if (m_hdmi && m_hdmi->signalPresent() && m_currentFormat.isValid())
        m_retryTimer.start();
}

void LowLatencyVideoPipelineController::retryCurrentFormat()
{
    if (m_hdmi && m_hdmi->signalPresent() && m_currentFormat.isValid())
        startForFormat(m_currentFormat);
}

void LowLatencyVideoPipelineController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void LowLatencyVideoPipelineController::setLastError(const QString &errorText)
{
    if (m_lastError == errorText)
        return;
    m_lastError = errorText;
    emit lastErrorChanged();
}
