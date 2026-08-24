#include "hdmirxcontroller.h"

#include <QFile>
#include <QSocketNotifier>
#include <QtMath>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/rk_hdmirx_config.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr char kVideoDevice[] = "/dev/video80";
constexpr char kEdidPath[] = "/sys/class/hdmirx/hdmirx/edid";
constexpr char kLowLatencyPath[] =
        "/sys/module/rockchip_hdmirx/parameters/low_latency";

int greatestCommonDivisor(int a, int b)
{
    while (b != 0) {
        const int remainder = a % b;
        a = b;
        b = remainder;
    }
    return a == 0 ? 1 : qAbs(a);
}

void normalizedFrameRate(double rawRate, int *numerator, int *denominator)
{
    struct KnownRate { double rate; int numerator; int denominator; };
    static const KnownRate knownRates[] = {
        {23.976, 24000, 1001}, {24.0, 24, 1}, {25.0, 25, 1},
        {29.970, 30000, 1001}, {30.0, 30, 1}, {50.0, 50, 1},
        {59.940, 60000, 1001}, {60.0, 60, 1},
        {100.0, 100, 1}, {119.880, 120000, 1001}, {120.0, 120, 1}
    };

    for (const KnownRate &known : knownRates) {
        if (qAbs(rawRate - known.rate) < 0.12) {
            *numerator = known.numerator;
            *denominator = known.denominator;
            return;
        }
    }

    int scaled = qMax(1, qRound(rawRate * 1000.0));
    int divisor = greatestCommonDivisor(scaled, 1000);
    *numerator = scaled / divisor;
    *denominator = 1000 / divisor;
}

QString fourccText(quint32 value)
{
    char text[5] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
        '\0'
    };
    return QString::fromLatin1(text);
}

QString gstFormatForFourcc(quint32 value)
{
    switch (value) {
    case V4L2_PIX_FMT_NV16:
        return QStringLiteral("NV16");
    case V4L2_PIX_FMT_NV24:
        return QStringLiteral("NV24");
    case V4L2_PIX_FMT_BGR24:
        return QStringLiteral("BGR");
    case V4L2_PIX_FMT_RGB24:
        return QStringLiteral("RGB");
    default:
        return QString();
    }
}

QString colorimetryForColorspace(quint32 value)
{
    switch (value) {
    case V4L2_COLORSPACE_REC709:
        return QStringLiteral("bt709");
    case V4L2_COLORSPACE_SMPTE170M:
        return QStringLiteral("bt601");
#ifdef V4L2_COLORSPACE_BT2020
    case V4L2_COLORSPACE_BT2020:
        return QStringLiteral("bt2020");
#endif
    default:
        return QStringLiteral("unknown");
    }
}

QString errnoText(const QString &operation)
{
    return QStringLiteral("%1: %2").arg(operation,
                                        QString::fromLocal8Bit(std::strerror(errno)));
}

} // namespace

HdmiRxController::HdmiRxController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<VideoFormat>("VideoFormat");
    m_probeTimer.setInterval(1000);
    connect(&m_probeTimer, &QTimer::timeout,
            this, &HdmiRxController::pollDevice);
}

HdmiRxController::~HdmiRxController()
{
    stop();
}

QString HdmiRxController::stateText() const
{
    switch (m_state) {
    case Boot: return tr("启动中");
    case NoSignal: return tr("无信号");
    case Probing: return tr("正在锁定信号");
    case Ready: return tr("信号已锁定");
    case Error: return tr("HDMI错误");
    }
    return tr("未知状态");
}

QString HdmiRxController::frameRateText() const
{
    if (!m_currentFormat.isValid())
        return QStringLiteral("--");

    const double rate = static_cast<double>(m_currentFormat.fpsNumerator)
            / m_currentFormat.fpsDenominator;
    return qFuzzyCompare(rate, qRound(rate))
            ? QString::number(qRound(rate))
            : QString::number(rate, 'f', 2);
}

void HdmiRxController::start()
{
    if (m_running)
        return;

    m_running = true;
    m_deviceMonitoringPaused = false;
    setState(Boot);

    bool edidChanged = false;
    m_safetyReady = ensureSafetySettings(&edidChanged);
    openDevice();
    m_probeTimer.start();

    QTimer::singleShot(edidChanged ? 1500 : 0,
                       this, &HdmiRxController::pollDevice);
}

void HdmiRxController::stop()
{
    m_running = false;
    m_deviceMonitoringPaused = false;
    m_probeTimer.stop();
    closeDevice();
}

void HdmiRxController::probeNow()
{
    pollDevice();
}

void HdmiRxController::pauseDeviceMonitoring()
{
    if (m_deviceMonitoringPaused)
        return;

    m_deviceMonitoringPaused = true;
    m_probeTimer.stop();
    closeDevice();
}

void HdmiRxController::resumeDeviceMonitoring()
{
    if (!m_running || !m_deviceMonitoringPaused)
        return;

    m_deviceMonitoringPaused = false;
    m_safetyReady = ensureSafetySettings();
    if (!m_safetyReady)
        return;

    openDevice();
    m_probeTimer.start();
    QTimer::singleShot(0, this, &HdmiRxController::pollDevice);
}

bool HdmiRxController::setLowLatencyEnabled(bool enabled)
{
    const bool ready = ensureTextValue(
                QString::fromLatin1(kLowLatencyPath),
                enabled ? QByteArrayLiteral("Y") : QByteArrayLiteral("N"),
                enabled ? QByteArrayLiteral("1\n") : QByteArrayLiteral("0\n"));
    m_safetyReady = !enabled && ready;
    return ready;
}

bool HdmiRxController::ensureSafetySettings(bool *edidChanged)
{
    bool localEdidChanged = false;
    const bool lowLatencySafe = ensureTextValue(
                QString::fromLatin1(kLowLatencyPath), QByteArrayLiteral("N"),
                QByteArrayLiteral("0\n"));
    const bool edidReady = ensureTextValue(
                QString::fromLatin1(kEdidPath), QByteArrayLiteral("2"),
                QByteArrayLiteral("2\n"), &localEdidChanged);

    if (edidChanged)
        *edidChanged = localEdidChanged;

    if (!lowLatencySafe || !edidReady) {
        setState(Error);
        return false;
    }

    return true;
}

bool HdmiRxController::ensureTextValue(const QString &path,
                                       const QByteArray &expected,
                                       const QByteArray &replacement,
                                       bool *changed)
{
    if (changed)
        *changed = false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setLastError(tr("无法读取 %1: %2").arg(path, file.errorString()));
        return false;
    }

    const QByteArray current = file.readAll().trimmed();
    file.close();
    if (current == expected)
        return true;

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setLastError(tr("无法写入 %1: %2").arg(path, file.errorString()));
        return false;
    }

    if (file.write(replacement) != replacement.size()) {
        setLastError(tr("写入 %1 不完整: %2").arg(path, file.errorString()));
        return false;
    }
    file.close();

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setLastError(tr("无法回读 %1: %2").arg(path, file.errorString()));
        return false;
    }

    const QByteArray verified = file.readAll().trimmed();
    file.close();
    if (verified != expected) {
        setLastError(tr("%1 回读值为 %2，期望值为 %3")
                     .arg(path, QString::fromLatin1(verified),
                          QString::fromLatin1(expected)));
        return false;
    }

    if (changed)
        *changed = true;
    return true;
}

bool HdmiRxController::openDevice()
{
    if (m_fd >= 0)
        return true;

    m_fd = ::open(kVideoDevice, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        setLastError(errnoText(tr("打开 /dev/video80 失败")));
        setState(Error);
        return false;
    }

    v4l2_event_subscription subscription = {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    ::ioctl(m_fd, VIDIOC_SUBSCRIBE_EVENT, &subscription);

    subscription = {};
    subscription.type = RK_HDMIRX_V4L2_EVENT_SIGNAL_LOST;
    ::ioctl(m_fd, VIDIOC_SUBSCRIBE_EVENT, &subscription);

    m_eventNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Exception, this);
    connect(m_eventNotifier, &QSocketNotifier::activated,
            this, &HdmiRxController::processDeviceEvents);
    return true;
}

void HdmiRxController::closeDevice()
{
    delete m_eventNotifier;
    m_eventNotifier = nullptr;

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

HdmiRxController::QueryResult HdmiRxController::queryFormat(
        VideoFormat *format, QString *errorText)
{
    if (m_fd < 0 && !openDevice()) {
        if (errorText)
            *errorText = m_lastError;
        return QueryFailed;
    }

    v4l2_dv_timings timings = {};
    if (::ioctl(m_fd, VIDIOC_QUERY_DV_TIMINGS, &timings) < 0) {
        if (errorText)
            *errorText = errnoText(tr("查询HDMI时序失败"));
        switch (errno) {
        case EAGAIN:
        case ENODATA:
        case ENOLCK:
        case ENOLINK:
            return SignalUnavailable;
        default:
            return QueryFailed;
        }
    }

    v4l2_format v4l2Format = {};
    v4l2Format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (::ioctl(m_fd, VIDIOC_G_FMT, &v4l2Format) < 0) {
        if (errorText)
            *errorText = errnoText(tr("查询采集格式失败"));
        return QueryFailed;
    }

    const v4l2_bt_timings &bt = timings.bt;
    const quint64 totalWidth = bt.width + bt.hfrontporch
            + bt.hsync + bt.hbackporch;
    quint64 totalHeight = bt.height + bt.vfrontporch
            + bt.vsync + bt.vbackporch;
    if (bt.interlaced)
        totalHeight += bt.il_vfrontporch + bt.il_vsync + bt.il_vbackporch;

    if (totalWidth == 0 || totalHeight == 0 || bt.pixelclock == 0) {
        if (errorText)
            *errorText = tr("HDMI时序尚未稳定");
        return SignalUnavailable;
    }

    VideoFormat detected;
    detected.width = static_cast<int>(bt.width);
    detected.height = static_cast<int>(bt.height);
    detected.driverFormat = fourccText(v4l2Format.fmt.pix_mp.pixelformat);
    detected.gstFormat = gstFormatForFourcc(v4l2Format.fmt.pix_mp.pixelformat);
    detected.colorimetry = colorimetryForColorspace(
                v4l2Format.fmt.pix_mp.colorspace);

    const double rawRate = static_cast<double>(bt.pixelclock)
            / static_cast<double>(totalWidth * totalHeight);
    normalizedFrameRate(rawRate, &detected.fpsNumerator,
                        &detected.fpsDenominator);

    if (!detected.isValid()) {
        if (errorText) {
            *errorText = tr("暂不支持采集格式 %1")
                    .arg(detected.driverFormat);
        }
        return QueryFailed;
    }

    *format = detected;
    return FormatReady;
}

void HdmiRxController::pollDevice()
{
    if (!m_running || m_deviceMonitoringPaused)
        return;

    if (!m_safetyReady)
        m_safetyReady = ensureSafetySettings();
    if (!m_safetyReady)
        return;

    VideoFormat detected;
    QString errorText;
    const QueryResult result = queryFormat(&detected, &errorText);
    if (result == SignalUnavailable) {
        markNoSignal();
        return;
    }
    if (result == QueryFailed) {
        markError(errorText);
        return;
    }

    acceptCandidate(detected);
}

void HdmiRxController::processDeviceEvents()
{
    if (m_fd < 0)
        return;

    for (;;) {
        v4l2_event event = {};
        if (::ioctl(m_fd, VIDIOC_DQEVENT, &event) < 0) {
            if (errno != EAGAIN)
                setLastError(errnoText(tr("读取HDMI事件失败")));
            break;
        }

        if (event.type == RK_HDMIRX_V4L2_EVENT_SIGNAL_LOST) {
            markNoSignal();
        } else if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
            m_candidateSamples = 0;
            setState(Probing);
            QTimer::singleShot(350, this, &HdmiRxController::pollDevice);
        }
    }
}

void HdmiRxController::acceptCandidate(const VideoFormat &format)
{
    if (format != m_candidateFormat) {
        m_candidateFormat = format;
        m_candidateSamples = 1;
        setState(Probing);
        return;
    }

    if (m_candidateSamples < 2)
        ++m_candidateSamples;
    if (m_candidateSamples < 2)
        return;

    const bool signalChanged = !m_signalPresent;
    const bool formatChangedValue = format != m_currentFormat;
    m_signalPresent = true;

    if (signalChanged)
        emit signalPresentChanged();

    if (formatChangedValue) {
        m_currentFormat = format;
        emit formatPropertiesChanged();
        emit formatChanged(m_currentFormat);
    }

    setLastError(QString());
    setState(Ready);
}

void HdmiRxController::markNoSignal()
{
    m_candidateSamples = 0;
    m_candidateFormat = VideoFormat();

    if (m_signalPresent) {
        m_signalPresent = false;
        m_currentFormat = VideoFormat();
        emit signalPresentChanged();
        emit formatPropertiesChanged();
        emit signalLost();
    }

    setLastError(QString());
    setState(NoSignal);
}

void HdmiRxController::markError(const QString &errorText)
{
    m_candidateSamples = 0;
    m_candidateFormat = VideoFormat();

    if (m_signalPresent) {
        m_signalPresent = false;
        m_currentFormat = VideoFormat();
        emit signalPresentChanged();
        emit formatPropertiesChanged();
        emit signalLost();
    }

    setLastError(errorText);
    setState(Error);
}

void HdmiRxController::setState(HdmiRxController::State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void HdmiRxController::setLastError(const QString &errorText)
{
    if (m_lastError == errorText)
        return;
    m_lastError = errorText;
    emit lastErrorChanged();
}
