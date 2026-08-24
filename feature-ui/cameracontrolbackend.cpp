#include "cameracontrolbackend.h"

UnavailableCameraControlBackend::UnavailableCameraControlBackend(QObject *parent)
    : ICameraControlBackend(parent)
{
}

QString UnavailableCameraControlBackend::connectionText() const
{
    return tr("Sony SDK 未安装");
}

void UnavailableCameraControlBackend::reconnect()
{
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setRecording(bool recording)
{
    Q_UNUSED(recording)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setIso(qulonglong value)
{
    Q_UNUSED(value)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setShutterSpeed(qulonglong value)
{
    Q_UNUSED(value)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setAperture(qulonglong value)
{
    Q_UNUSED(value)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setWhiteBalance(qulonglong value)
{
    Q_UNUSED(value)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setFocusMode(qulonglong value)
{
    Q_UNUSED(value)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::setAutoFocus(bool pressed)
{
    Q_UNUSED(pressed)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::touchFocus(qreal normalizedX,
                                                 qreal normalizedY)
{
    Q_UNUSED(normalizedX)
    Q_UNUSED(normalizedY)
    rejectUnavailable();
}

void UnavailableCameraControlBackend::rejectUnavailable()
{
    const QString errorText = tr("未安装 Sony Camera Remote SDK，控制命令未发送");
    if (m_lastError == errorText)
        return;

    m_lastError = errorText;
    emit lastErrorChanged();
}
