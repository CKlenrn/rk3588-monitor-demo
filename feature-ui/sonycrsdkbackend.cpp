#include "sonycrsdkbackend.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wreorder"
#include <CameraRemote_SDK.h>
#include <IDeviceCallback.h>
#pragma GCC diagnostic pop

#include <QMetaObject>
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>

namespace {

constexpr int reconnectDelayMs = 2000;
constexpr int propertyPollMs = 1000;
constexpr int connectionTimeoutMs = 5000;
constexpr int disconnectionTimeoutMs = 3000;

QString sdkErrorText(const QString &operation, SCRSDK::CrError error)
{
    return QStringLiteral("%1失败（Sony SDK 0x%2）")
        .arg(operation)
        .arg(static_cast<quint32>(error), 8, 16, QLatin1Char('0'));
}

QVariantMap option(quint64 value, const QString &label)
{
    QVariantMap result;
    result.insert(QStringLiteral("value"), QVariant::fromValue<qulonglong>(value));
    result.insert(QStringLiteral("label"), label);
    return result;
}

QVector<quint64> propertyValues(const SCRSDK::CrDeviceProperty &property)
{
    QVector<quint64> result;
    const auto *data = property.GetSetValues();
    const quint32 byteCount = property.GetSetValueSize();
    if (!data || byteCount == 0)
        return result;

    const int type = static_cast<int>(property.GetValueType()) & 0x100f;
    int valueSize = 0;
    switch (type) {
    case SCRSDK::CrDataType_UInt8:
    case SCRSDK::CrDataType_Int8:
        valueSize = 1;
        break;
    case SCRSDK::CrDataType_UInt16:
    case SCRSDK::CrDataType_Int16:
        valueSize = 2;
        break;
    case SCRSDK::CrDataType_UInt32:
    case SCRSDK::CrDataType_Int32:
        valueSize = 4;
        break;
    case SCRSDK::CrDataType_UInt64:
        valueSize = 8;
        break;
    default:
        return result;
    }

    const quint32 count = byteCount / static_cast<quint32>(valueSize);
    result.reserve(static_cast<int>(count));
    for (quint32 index = 0; index < count; ++index) {
        quint64 value = 0;
        std::memcpy(&value, data + index * valueSize,
                    static_cast<size_t>(valueSize));
        result.append(value);
    }
    return result;
}

QString isoLabel(quint64 value)
{
    const quint32 raw = static_cast<quint32>(value);
    const quint32 sensitivity = raw & 0x00ffffffu;
    if (sensitivity == SCRSDK::CrISO_AUTO)
        return QStringLiteral("ISO AUTO");
    return QStringLiteral("ISO %1").arg(sensitivity);
}

QString apertureLabel(quint64 value)
{
    const quint16 raw = static_cast<quint16>(value);
    if (raw == SCRSDK::CrFnumber_IrisClose)
        return QStringLiteral("CLOSE");
    if (raw >= SCRSDK::CrFnumber_Unknown)
        return QStringLiteral("--");
    return QStringLiteral("F%1").arg(raw / 100.0, 0, 'f', raw % 10 ? 2 : 1);
}

QString shutterLabel(quint64 value)
{
    const quint32 raw = static_cast<quint32>(value);
    if (raw == SCRSDK::CrShutterSpeed_Bulb)
        return QStringLiteral("BULB");
    if (raw == SCRSDK::CrShutterSpeed_Nothing)
        return QStringLiteral("--");

    const quint32 numerator = raw >> 16;
    const quint32 denominator = raw & 0xffffu;
    if (denominator == 0)
        return QStringLiteral("--");
    if (numerator == 1)
        return QStringLiteral("1/%1").arg(denominator);
    return QStringLiteral("%1 s").arg(numerator / static_cast<double>(denominator),
                                      0, 'g', 4);
}

QString whiteBalanceLabel(quint64 value)
{
    switch (static_cast<quint16>(value)) {
    case SCRSDK::CrWhiteBalance_AWB: return QStringLiteral("AWB");
    case SCRSDK::CrWhiteBalance_Underwater_Auto: return QStringLiteral("水下自动");
    case SCRSDK::CrWhiteBalance_Daylight: return QStringLiteral("日光");
    case SCRSDK::CrWhiteBalance_Shadow: return QStringLiteral("阴影");
    case SCRSDK::CrWhiteBalance_Cloudy: return QStringLiteral("阴天");
    case SCRSDK::CrWhiteBalance_Tungsten: return QStringLiteral("白炽灯");
    case SCRSDK::CrWhiteBalance_Fluorescent: return QStringLiteral("荧光灯");
    case SCRSDK::CrWhiteBalance_Fluorescent_WarmWhite: return QStringLiteral("暖白荧光");
    case SCRSDK::CrWhiteBalance_Fluorescent_CoolWhite: return QStringLiteral("冷白荧光");
    case SCRSDK::CrWhiteBalance_Fluorescent_DayWhite: return QStringLiteral("日白荧光");
    case SCRSDK::CrWhiteBalance_Fluorescent_Daylight: return QStringLiteral("日光荧光");
    case SCRSDK::CrWhiteBalance_Flush: return QStringLiteral("闪光灯");
    case SCRSDK::CrWhiteBalance_ColorTemp: return QStringLiteral("色温");
    case SCRSDK::CrWhiteBalance_Custom_1: return QStringLiteral("自定义 1");
    case SCRSDK::CrWhiteBalance_Custom_2: return QStringLiteral("自定义 2");
    case SCRSDK::CrWhiteBalance_Custom_3: return QStringLiteral("自定义 3");
    case SCRSDK::CrWhiteBalance_Custom: return QStringLiteral("自定义");
    default: return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0'));
    }
}

QString focusModeLabel(quint64 value)
{
    switch (static_cast<quint16>(value)) {
    case SCRSDK::CrFocus_MF: return QStringLiteral("MF");
    case SCRSDK::CrFocus_AF_S: return QStringLiteral("AF-S");
    case SCRSDK::CrFocus_AF_C: return QStringLiteral("AF-C");
    case SCRSDK::CrFocus_AF_A: return QStringLiteral("AF-A");
    case SCRSDK::CrFocus_AF_D: return QStringLiteral("AF-D");
    case SCRSDK::CrFocus_DMF: return QStringLiteral("DMF");
    case SCRSDK::CrFocus_PF: return QStringLiteral("PF");
    default: return QStringLiteral("0x%1").arg(value, 4, 16, QLatin1Char('0'));
    }
}

template<typename Formatter>
QVariantList optionsFor(const SCRSDK::CrDeviceProperty &property,
                        Formatter formatter)
{
    QVariantList result;
    if (!property.IsSetEnableCurrentValue())
        return result;
    for (quint64 value : propertyValues(property))
        result.append(option(value, formatter(value)));
    return result;
}

QString propertyString(const SCRSDK::CrDeviceProperty &property)
{
    auto *data = property.GetCurrentStr();
    if (!data || data[0] == 0)
        return {};
    return QString::fromUtf16(reinterpret_cast<const ushort *>(data + 1), data[0]);
}

} // namespace

class SonyDeviceCallback;

class SonyCrSdkWorker final : public QObject
{
    Q_OBJECT

public:
    explicit SonyCrSdkWorker(QObject *parent = nullptr);
    ~SonyCrSdkWorker() override;

    void postDisconnected(quint32 error);
    void postPropertyChanged();
    void postSdkError(quint32 error);

public slots:
    void start();
    void reconnect();
    void shutdown();
    void refreshProperties();
    void setRecording(bool recording);
    void setIso(qulonglong value);
    void setShutterSpeed(qulonglong value);
    void setAperture(qulonglong value);
    void setWhiteBalance(qulonglong value);
    void setFocusMode(qulonglong value);
    void setAutoFocus(bool pressed);
    void touchFocus(qreal normalizedX, qreal normalizedY);

signals:
    void statusChanged(int state, const QString &text, const QString &error);
    void snapshotReady(const QVariantMap &snapshot);
    void commandError(const QString &error);

private:
    void scheduleReconnect();
    void disconnectCurrent();
    void setProperty(quint32 code, quint64 value, const QString &name,
                     bool validateAgainstOptions = true);

    std::unique_ptr<SonyDeviceCallback> m_callback;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_propertyTimer = nullptr;
    SCRSDK::CrDeviceHandle m_deviceHandle = 0;
    QString m_modelName;
    bool m_sdkInitialized = false;
    bool m_connected = false;
    bool m_shuttingDown = false;
};

class SonyDeviceCallback final : public SCRSDK::IDeviceCallback
{
public:
    explicit SonyDeviceCallback(SonyCrSdkWorker *owner) : m_owner(owner) {}

    void prepareConnectionWait()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waitingForConnection = true;
        m_connectionComplete = false;
        m_connectionSucceeded = false;
        m_connectionError = 0;
    }

    bool waitForConnection(quint32 *error)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const bool completed = m_condition.wait_for(
            lock, std::chrono::milliseconds(connectionTimeoutMs),
            [this]() { return m_connectionComplete; });
        m_waitingForConnection = false;
        if (error)
            *error = m_connectionError;
        return completed && m_connectionSucceeded;
    }

    void OnConnected(SCRSDK::DeviceConnectionVersioin) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_waitingForConnection)
            return;
        m_connectionSucceeded = true;
        m_connectionComplete = true;
        m_condition.notify_all();
    }

    void OnDisconnected(CrInt32u error) override
    {
        m_owner->postDisconnected(error);
    }

    void OnPropertyChanged() override
    {
        m_owner->postPropertyChanged();
    }

    void OnPropertyChangedCodes(CrInt32u, CrInt32u *) override
    {
        m_owner->postPropertyChanged();
    }

    void OnError(CrInt32u error) override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_waitingForConnection) {
                m_connectionError = error;
                m_connectionSucceeded = false;
                m_connectionComplete = true;
                m_condition.notify_all();
                return;
            }
        }
        m_owner->postSdkError(error);
    }

private:
    SonyCrSdkWorker *m_owner = nullptr;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_waitingForConnection = false;
    bool m_connectionComplete = false;
    bool m_connectionSucceeded = false;
    quint32 m_connectionError = 0;
};

SonyCrSdkWorker::SonyCrSdkWorker(QObject *parent)
    : QObject(parent),
      m_callback(new SonyDeviceCallback(this)),
      m_reconnectTimer(new QTimer(this)),
      m_propertyTimer(new QTimer(this))
{
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(reconnectDelayMs);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &SonyCrSdkWorker::reconnect);

    m_propertyTimer->setInterval(propertyPollMs);
    connect(m_propertyTimer, &QTimer::timeout,
            this, &SonyCrSdkWorker::refreshProperties);
}

SonyCrSdkWorker::~SonyCrSdkWorker() = default;

void SonyCrSdkWorker::start()
{
    if (m_shuttingDown || m_sdkInitialized)
        return;
    if (!SCRSDK::Init()) {
        emit statusChanged(ICameraControlBackend::CameraError,
                           tr("Sony SDK 初始化失败"),
                           tr("请检查发布目录中的 Sony 运行库"));
        scheduleReconnect();
        return;
    }
    m_sdkInitialized = true;
    reconnect();
}

void SonyCrSdkWorker::reconnect()
{
    if (m_shuttingDown)
        return;
    if (!m_sdkInitialized) {
        start();
        return;
    }

    m_reconnectTimer->stop();
    disconnectCurrent();
    emit statusChanged(ICameraControlBackend::Discovering,
                       tr("正在查找 USB FX30"), QString());

    SCRSDK::ICrEnumCameraObjectInfo *cameraList = nullptr;
    const SCRSDK::CrError enumError = SCRSDK::EnumCameraObjects(&cameraList, 3);
    if (enumError || !cameraList) {
        emit statusChanged(ICameraControlBackend::Disconnected,
                           tr("未发现 USB FX30"),
                           enumError ? sdkErrorText(tr("枚举相机"), enumError)
                                     : QString());
        scheduleReconnect();
        return;
    }

    SCRSDK::ICrCameraObjectInfo *selected = nullptr;
    QString otherUsbModel;
    for (quint32 index = 0; index < cameraList->GetCount(); ++index) {
        auto *candidate = cameraList->GetCameraObjectInfo(index);
        if (!candidate)
            continue;
        const QString connection = QString::fromLocal8Bit(
            candidate->GetConnectionTypeName());
        if (connection.compare(QStringLiteral("USB"), Qt::CaseInsensitive) != 0)
            continue;
        const QString model = QString::fromLocal8Bit(candidate->GetModel());
        if (model.contains(QStringLiteral("FX30"), Qt::CaseInsensitive)
                || model.contains(QStringLiteral("ILME-FX30"), Qt::CaseInsensitive)) {
            selected = const_cast<SCRSDK::ICrCameraObjectInfo *>(candidate);
            m_modelName = model;
            break;
        }
        if (otherUsbModel.isEmpty())
            otherUsbModel = model;
    }

    if (!selected) {
        cameraList->Release();
        const QString detail = otherUsbModel.isEmpty()
            ? QString()
            : tr("检测到其他 USB 相机：%1").arg(otherUsbModel);
        emit statusChanged(ICameraControlBackend::Disconnected,
                           tr("未发现 USB FX30"), detail);
        scheduleReconnect();
        return;
    }

    emit statusChanged(ICameraControlBackend::Connecting,
                       tr("正在连接 %1").arg(m_modelName), QString());
    m_callback->prepareConnectionWait();
    SCRSDK::CrDeviceHandle handle = 0;
    const SCRSDK::CrError connectError = SCRSDK::Connect(
        selected, m_callback.get(), &handle, SCRSDK::CrSdkControlMode_Remote,
        SCRSDK::CrReconnecting_ON, "", "", "", 0);
    cameraList->Release();

    if (connectError) {
        emit statusChanged(ICameraControlBackend::CameraError,
                           tr("FX30 连接失败"),
                           sdkErrorText(tr("连接相机"), connectError));
        scheduleReconnect();
        return;
    }

    m_deviceHandle = handle;
    quint32 callbackError = 0;
    if (!m_callback->waitForConnection(&callbackError)) {
        disconnectCurrent();
        emit statusChanged(ICameraControlBackend::CameraError,
                           tr("FX30 连接超时"),
                           callbackError
                               ? sdkErrorText(tr("连接回调"), callbackError)
                               : tr("5 秒内未收到 Sony SDK 连接完成事件"));
        scheduleReconnect();
        return;
    }

    m_connected = true;
    emit statusChanged(ICameraControlBackend::Ready,
                       tr("USB 已连接"), QString());
    m_propertyTimer->start();
    refreshProperties();
}

void SonyCrSdkWorker::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    m_reconnectTimer->stop();
    m_propertyTimer->stop();
    disconnectCurrent();
    if (m_sdkInitialized) {
        SCRSDK::Release();
        m_sdkInitialized = false;
    }
}

void SonyCrSdkWorker::scheduleReconnect()
{
    if (!m_shuttingDown && !m_reconnectTimer->isActive())
        m_reconnectTimer->start();
}

void SonyCrSdkWorker::disconnectCurrent()
{
    m_propertyTimer->stop();
    m_connected = false;
    if (!m_deviceHandle)
        return;
    SCRSDK::Disconnect(m_deviceHandle);
    QThread::msleep(disconnectionTimeoutMs);
    SCRSDK::ReleaseDevice(m_deviceHandle);
    m_deviceHandle = 0;
}

void SonyCrSdkWorker::refreshProperties()
{
    if (!m_connected || !m_deviceHandle)
        return;

    SCRSDK::CrDeviceProperty *properties = nullptr;
    CrInt32 propertyCount = 0;
    const SCRSDK::CrError error = SCRSDK::GetDeviceProperties(
        m_deviceHandle, &properties, &propertyCount);
    if (error || !properties) {
        emit commandError(sdkErrorText(tr("读取相机属性"), error));
        return;
    }

    QVariantMap snapshot;
    snapshot.insert(QStringLiteral("modelName"), m_modelName);
    bool hasRecordingState = false;
    bool hasAutoFocus = false;
    bool hasRemoteTouch = false;
    bool remoteTouchEnabled = false;

    for (CrInt32 index = 0; index < propertyCount; ++index) {
        const SCRSDK::CrDeviceProperty &property = properties[index];
        const quint32 code = property.GetCode();
        const quint64 current = property.GetCurrentValue();
        switch (code) {
        case SCRSDK::CrDeviceProperty_SoftwareVersion:
            snapshot.insert(QStringLiteral("firmwareVersion"),
                            propertyString(property));
            break;
        case SCRSDK::CrDeviceProperty_RecordingState:
            hasRecordingState = true;
            snapshot.insert(QStringLiteral("recording"),
                            current == SCRSDK::CrMovie_Recording_State_Recording);
            break;
        case SCRSDK::CrDeviceProperty_IsoSensitivity:
            snapshot.insert(QStringLiteral("isoValue"),
                            QVariant::fromValue<qulonglong>(current));
            snapshot.insert(QStringLiteral("isoOptions"),
                            optionsFor(property, isoLabel));
            break;
        case SCRSDK::CrDeviceProperty_ShutterSpeed:
            snapshot.insert(QStringLiteral("shutterValue"),
                            QVariant::fromValue<qulonglong>(current));
            snapshot.insert(QStringLiteral("shutterOptions"),
                            optionsFor(property, shutterLabel));
            break;
        case SCRSDK::CrDeviceProperty_FNumber:
            snapshot.insert(QStringLiteral("apertureValue"),
                            QVariant::fromValue<qulonglong>(current));
            snapshot.insert(QStringLiteral("apertureOptions"),
                            optionsFor(property, apertureLabel));
            break;
        case SCRSDK::CrDeviceProperty_WhiteBalance:
            snapshot.insert(QStringLiteral("whiteBalanceValue"),
                            QVariant::fromValue<qulonglong>(current));
            snapshot.insert(QStringLiteral("whiteBalanceOptions"),
                            optionsFor(property, whiteBalanceLabel));
            break;
        case SCRSDK::CrDeviceProperty_FocusMode:
            snapshot.insert(QStringLiteral("focusModeValue"),
                            QVariant::fromValue<qulonglong>(current));
            snapshot.insert(QStringLiteral("focusModeOptions"),
                            optionsFor(property, focusModeLabel));
            break;
        case SCRSDK::CrDeviceProperty_PushAutoFocus:
            hasAutoFocus = property.IsSetEnableCurrentValue();
            break;
        case SCRSDK::CrDeviceProperty_RemoteTouchOperation:
            hasRemoteTouch = property.IsSetEnableCurrentValue();
            break;
        case SCRSDK::CrDeviceProperty_RemoteTouchOperationEnableStatus:
            remoteTouchEnabled =
                current == SCRSDK::CrRemoteTouchOperation_Enable;
            break;
        default:
            break;
        }
    }
    SCRSDK::ReleaseDeviceProperties(m_deviceHandle, properties);

    const bool exposure = !snapshot.value(QStringLiteral("isoOptions")).toList().isEmpty()
        || !snapshot.value(QStringLiteral("shutterOptions")).toList().isEmpty()
        || !snapshot.value(QStringLiteral("apertureOptions")).toList().isEmpty();
    const bool whiteBalance =
        !snapshot.value(QStringLiteral("whiteBalanceOptions")).toList().isEmpty();
    const bool focusMode =
        !snapshot.value(QStringLiteral("focusModeOptions")).toList().isEmpty();
    snapshot.insert(QStringLiteral("canRecord"), hasRecordingState);
    snapshot.insert(QStringLiteral("canControlExposure"), exposure);
    snapshot.insert(QStringLiteral("canControlWhiteBalance"), whiteBalance);
    snapshot.insert(QStringLiteral("canAutoFocus"), hasAutoFocus);
    snapshot.insert(QStringLiteral("canTouchFocus"),
                    hasRemoteTouch && remoteTouchEnabled);
    snapshot.insert(QStringLiteral("canControlFocus"),
                    focusMode || hasAutoFocus || (hasRemoteTouch && remoteTouchEnabled));
    emit snapshotReady(snapshot);
}

void SonyCrSdkWorker::setProperty(quint32 code, quint64 value,
                                  const QString &name,
                                  bool validateAgainstOptions)
{
    if (!m_connected || !m_deviceHandle) {
        emit commandError(tr("FX30 未连接，%1未发送").arg(name));
        return;
    }

    SCRSDK::CrDeviceProperty *properties = nullptr;
    CrInt32 count = 0;
    SCRSDK::CrError error = SCRSDK::GetSelectDeviceProperties(
        m_deviceHandle, 1, &code, &properties, &count);
    if (error || !properties || count < 1) {
        if (properties)
            SCRSDK::ReleaseDeviceProperties(m_deviceHandle, properties);
        emit commandError(sdkErrorText(tr("读取%1能力").arg(name), error));
        return;
    }

    SCRSDK::CrDeviceProperty property = properties[0];
    SCRSDK::ReleaseDeviceProperties(m_deviceHandle, properties);
    if (!property.IsSetEnableCurrentValue()) {
        emit commandError(tr("当前 FX30 状态不允许修改%1").arg(name));
        return;
    }
    if (validateAgainstOptions) {
        const QVector<quint64> allowed = propertyValues(property);
        if (!allowed.isEmpty() && !allowed.contains(value)) {
            emit commandError(tr("FX30 未返回该%1选项").arg(name));
            return;
        }
    }

    property.SetCurrentValue(value);
    error = SCRSDK::SetDeviceProperty(m_deviceHandle, &property);
    if (error) {
        emit commandError(sdkErrorText(tr("设置%1").arg(name), error));
        return;
    }
    QTimer::singleShot(250, this, &SonyCrSdkWorker::refreshProperties);
}

void SonyCrSdkWorker::setRecording(bool recording)
{
    if (!m_connected || !m_deviceHandle) {
        emit commandError(tr("FX30 未连接，录制命令未发送"));
        return;
    }
    const SCRSDK::CrError error = SCRSDK::SendCommand(
        m_deviceHandle, SCRSDK::CrCommandId_MovieRecord,
        recording ? SCRSDK::CrCommandParam_Down : SCRSDK::CrCommandParam_Up);
    if (error) {
        emit commandError(sdkErrorText(recording ? tr("开始录制")
                                                  : tr("停止录制"), error));
        return;
    }
    QTimer::singleShot(300, this, &SonyCrSdkWorker::refreshProperties);
}

void SonyCrSdkWorker::setIso(qulonglong value)
{
    setProperty(SCRSDK::CrDeviceProperty_IsoSensitivity, value, tr("ISO"));
}

void SonyCrSdkWorker::setShutterSpeed(qulonglong value)
{
    setProperty(SCRSDK::CrDeviceProperty_ShutterSpeed, value, tr("快门"));
}

void SonyCrSdkWorker::setAperture(qulonglong value)
{
    setProperty(SCRSDK::CrDeviceProperty_FNumber, value, tr("光圈"));
}

void SonyCrSdkWorker::setWhiteBalance(qulonglong value)
{
    setProperty(SCRSDK::CrDeviceProperty_WhiteBalance, value, tr("白平衡"));
}

void SonyCrSdkWorker::setFocusMode(qulonglong value)
{
    setProperty(SCRSDK::CrDeviceProperty_FocusMode, value, tr("对焦模式"));
}

void SonyCrSdkWorker::setAutoFocus(bool pressed)
{
    setProperty(SCRSDK::CrDeviceProperty_PushAutoFocus,
                pressed ? SCRSDK::CrPushAutoFocus_Down
                        : SCRSDK::CrPushAutoFocus_Up,
                tr("自动对焦"), false);
}

void SonyCrSdkWorker::touchFocus(qreal normalizedX, qreal normalizedY)
{
    const qreal x = qBound<qreal>(0.0, normalizedX, 1.0);
    const qreal y = qBound<qreal>(0.0, normalizedY, 1.0);
    const quint32 cameraX = static_cast<quint32>(qRound(x * 639.0));
    const quint32 cameraY = static_cast<quint32>(qRound(y * 479.0));
    setProperty(SCRSDK::CrDeviceProperty_RemoteTouchOperation,
                (cameraX << 16) | cameraY, tr("触控对焦"), false);
}

void SonyCrSdkWorker::postDisconnected(quint32 error)
{
    QMetaObject::invokeMethod(this, [this, error]() {
        if (m_shuttingDown || !m_connected)
            return;
        m_connected = false;
        m_propertyTimer->stop();
        if (m_deviceHandle) {
            SCRSDK::ReleaseDevice(m_deviceHandle);
            m_deviceHandle = 0;
        }
        emit statusChanged(ICameraControlBackend::Recovering,
                           tr("FX30 已断开，正在重连"),
                           error ? sdkErrorText(tr("相机断开"), error) : QString());
        scheduleReconnect();
    }, Qt::QueuedConnection);
}

void SonyCrSdkWorker::postPropertyChanged()
{
    QMetaObject::invokeMethod(this, "refreshProperties", Qt::QueuedConnection);
}

void SonyCrSdkWorker::postSdkError(quint32 error)
{
    QMetaObject::invokeMethod(this, [this, error]() {
        if (!m_shuttingDown)
            emit commandError(sdkErrorText(tr("相机操作"), error));
    }, Qt::QueuedConnection);
}

SonyCrSdkBackend::SonyCrSdkBackend(QObject *parent)
    : ICameraControlBackend(parent),
      m_thread(new QThread(this)),
      m_worker(new SonyCrSdkWorker)
{
    m_state = Discovering;
    m_connectionText = tr("正在启动 Sony SDK");
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &SonyCrSdkWorker::start);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SonyCrSdkWorker::statusChanged,
            this, &SonyCrSdkBackend::applyStatus);
    connect(m_worker, &SonyCrSdkWorker::snapshotReady,
            this, &SonyCrSdkBackend::applySnapshot);
    connect(m_worker, &SonyCrSdkWorker::commandError,
            this, &SonyCrSdkBackend::applyCommandError);
    m_thread->start();
}

SonyCrSdkBackend::~SonyCrSdkBackend()
{
    if (m_thread && m_thread->isRunning()) {
        QMetaObject::invokeMethod(m_worker, "shutdown",
                                  Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }
    m_worker = nullptr;
}

void SonyCrSdkBackend::reconnect()
{
    QMetaObject::invokeMethod(m_worker, "reconnect", Qt::QueuedConnection);
}

void SonyCrSdkBackend::setRecording(bool recording)
{
    QMetaObject::invokeMethod(m_worker, "setRecording", Qt::QueuedConnection,
                              Q_ARG(bool, recording));
}

void SonyCrSdkBackend::setIso(qulonglong value)
{
    QMetaObject::invokeMethod(m_worker, "setIso", Qt::QueuedConnection,
                              Q_ARG(qulonglong, value));
}

void SonyCrSdkBackend::setShutterSpeed(qulonglong value)
{
    QMetaObject::invokeMethod(m_worker, "setShutterSpeed", Qt::QueuedConnection,
                              Q_ARG(qulonglong, value));
}

void SonyCrSdkBackend::setAperture(qulonglong value)
{
    QMetaObject::invokeMethod(m_worker, "setAperture", Qt::QueuedConnection,
                              Q_ARG(qulonglong, value));
}

void SonyCrSdkBackend::setWhiteBalance(qulonglong value)
{
    QMetaObject::invokeMethod(m_worker, "setWhiteBalance", Qt::QueuedConnection,
                              Q_ARG(qulonglong, value));
}

void SonyCrSdkBackend::setFocusMode(qulonglong value)
{
    QMetaObject::invokeMethod(m_worker, "setFocusMode", Qt::QueuedConnection,
                              Q_ARG(qulonglong, value));
}

void SonyCrSdkBackend::setAutoFocus(bool pressed)
{
    QMetaObject::invokeMethod(m_worker, "setAutoFocus", Qt::QueuedConnection,
                              Q_ARG(bool, pressed));
}

void SonyCrSdkBackend::touchFocus(qreal normalizedX, qreal normalizedY)
{
    QMetaObject::invokeMethod(m_worker, "touchFocus", Qt::QueuedConnection,
                              Q_ARG(qreal, normalizedX),
                              Q_ARG(qreal, normalizedY));
}

void SonyCrSdkBackend::applyStatus(int state, const QString &text,
                                   const QString &error)
{
    qInfo().noquote() << "Sony camera:" << text;
    if (!error.isEmpty())
        qWarning().noquote() << "Sony camera:" << error;
    const ConnectionState nextState = static_cast<ConnectionState>(state);
    const bool nextConnected = nextState == Ready;
    const bool connectionChanged = m_state != nextState
        || m_connectionText != text || m_connected != nextConnected;
    m_state = nextState;
    m_connectionText = text;
    m_connected = nextConnected;
    if (connectionChanged)
        emit connectionStateChanged();

    if (m_lastError != error) {
        m_lastError = error;
        emit lastErrorChanged();
    }
    if (!nextConnected)
        clearCameraState();
}

void SonyCrSdkBackend::applySnapshot(const QVariantMap &snapshot)
{
    const QString model = snapshot.value(QStringLiteral("modelName"),
                                         QStringLiteral("--")).toString();
    const QString firmware = snapshot.value(QStringLiteral("firmwareVersion"),
                                            QStringLiteral("--")).toString();
    if (m_modelName != model || m_firmwareVersion != firmware) {
        m_modelName = model;
        m_firmwareVersion = firmware.isEmpty() ? QStringLiteral("--") : firmware;
        emit cameraInfoChanged();
    }

    const bool recordingNow = snapshot.value(QStringLiteral("recording")).toBool();
    if (m_recording != recordingNow) {
        m_recording = recordingNow;
        emit recordingChanged();
    }

    const bool canRecordNow = snapshot.value(QStringLiteral("canRecord")).toBool();
    const bool exposureNow = snapshot.value(QStringLiteral("canControlExposure")).toBool();
    const bool focusNow = snapshot.value(QStringLiteral("canControlFocus")).toBool();
    const bool whiteBalanceNow = snapshot.value(
        QStringLiteral("canControlWhiteBalance")).toBool();
    const bool autoFocusNow = snapshot.value(QStringLiteral("canAutoFocus")).toBool();
    const bool touchFocusNow = snapshot.value(QStringLiteral("canTouchFocus")).toBool();
    if (m_canRecord != canRecordNow
            || m_canControlExposure != exposureNow
            || m_canControlFocus != focusNow
            || m_canControlWhiteBalance != whiteBalanceNow
            || m_canAutoFocus != autoFocusNow
            || m_canTouchFocus != touchFocusNow) {
        m_canRecord = canRecordNow;
        m_canControlExposure = exposureNow;
        m_canControlFocus = focusNow;
        m_canControlWhiteBalance = whiteBalanceNow;
        m_canAutoFocus = autoFocusNow;
        m_canTouchFocus = touchFocusNow;
        emit capabilitiesChanged();
    }

    const QVariantList iso = snapshot.value(QStringLiteral("isoOptions")).toList();
    const QVariantList shutter = snapshot.value(QStringLiteral("shutterOptions")).toList();
    const QVariantList aperture = snapshot.value(QStringLiteral("apertureOptions")).toList();
    const QVariantList whiteBalance = snapshot.value(
        QStringLiteral("whiteBalanceOptions")).toList();
    const QVariantList focusMode = snapshot.value(
        QStringLiteral("focusModeOptions")).toList();
    const qulonglong isoValueNow = snapshot.value(QStringLiteral("isoValue")).toULongLong();
    const qulonglong shutterValueNow = snapshot.value(
        QStringLiteral("shutterValue")).toULongLong();
    const qulonglong apertureValueNow = snapshot.value(
        QStringLiteral("apertureValue")).toULongLong();
    const qulonglong whiteBalanceValueNow = snapshot.value(
        QStringLiteral("whiteBalanceValue")).toULongLong();
    const qulonglong focusModeValueNow = snapshot.value(
        QStringLiteral("focusModeValue")).toULongLong();
    if (m_isoOptions != iso || m_shutterOptions != shutter
            || m_apertureOptions != aperture
            || m_whiteBalanceOptions != whiteBalance
            || m_focusModeOptions != focusMode
            || m_isoValue != isoValueNow
            || m_shutterValue != shutterValueNow
            || m_apertureValue != apertureValueNow
            || m_whiteBalanceValue != whiteBalanceValueNow
            || m_focusModeValue != focusModeValueNow) {
        m_isoOptions = iso;
        m_shutterOptions = shutter;
        m_apertureOptions = aperture;
        m_whiteBalanceOptions = whiteBalance;
        m_focusModeOptions = focusMode;
        m_isoValue = isoValueNow;
        m_shutterValue = shutterValueNow;
        m_apertureValue = apertureValueNow;
        m_whiteBalanceValue = whiteBalanceValueNow;
        m_focusModeValue = focusModeValueNow;
        emit cameraSettingsChanged();
    }
}

void SonyCrSdkBackend::applyCommandError(const QString &error)
{
    qWarning().noquote() << "Sony camera:" << error;
    if (m_lastError == error)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

void SonyCrSdkBackend::clearCameraState()
{
    const bool infoChanged = m_modelName != QStringLiteral("--")
        || m_firmwareVersion != QStringLiteral("--");
    const bool recordingChangedNow = m_recording;
    const bool capabilitiesChangedNow = m_canRecord || m_canControlExposure
        || m_canControlFocus || m_canControlWhiteBalance
        || m_canAutoFocus || m_canTouchFocus;
    const bool settingsChangedNow = !m_isoOptions.isEmpty()
        || !m_shutterOptions.isEmpty() || !m_apertureOptions.isEmpty()
        || !m_whiteBalanceOptions.isEmpty() || !m_focusModeOptions.isEmpty();

    m_modelName = QStringLiteral("--");
    m_firmwareVersion = QStringLiteral("--");
    m_recording = false;
    m_canRecord = false;
    m_canControlExposure = false;
    m_canControlFocus = false;
    m_canControlWhiteBalance = false;
    m_canAutoFocus = false;
    m_canTouchFocus = false;
    m_isoOptions.clear();
    m_shutterOptions.clear();
    m_apertureOptions.clear();
    m_whiteBalanceOptions.clear();
    m_focusModeOptions.clear();
    m_isoValue = m_shutterValue = m_apertureValue = 0;
    m_whiteBalanceValue = m_focusModeValue = 0;
    if (infoChanged)
        emit cameraInfoChanged();
    if (recordingChangedNow)
        emit recordingChanged();
    if (capabilitiesChangedNow)
        emit capabilitiesChanged();
    if (settingsChangedNow)
        emit cameraSettingsChanged();
}

#include "sonycrsdkbackend.moc"
