#ifndef SONYCRSDKBACKEND_H
#define SONYCRSDKBACKEND_H

#include "cameracontrolbackend.h"

#include <QVariantMap>

class QThread;
class SonyCrSdkWorker;

class SonyCrSdkBackend final : public ICameraControlBackend
{
    Q_OBJECT

public:
    explicit SonyCrSdkBackend(QObject *parent = nullptr);
    ~SonyCrSdkBackend() override;

    ConnectionState connectionState() const override { return m_state; }
    QString connectionText() const override { return m_connectionText; }
    QString modelName() const override { return m_modelName; }
    QString firmwareVersion() const override { return m_firmwareVersion; }
    bool connected() const override { return m_connected; }
    bool recording() const override { return m_recording; }
    bool canRecord() const override { return m_canRecord; }
    bool canControlExposure() const override { return m_canControlExposure; }
    bool canControlFocus() const override { return m_canControlFocus; }
    bool canControlWhiteBalance() const override
    {
        return m_canControlWhiteBalance;
    }
    bool canAutoFocus() const override { return m_canAutoFocus; }
    bool canTouchFocus() const override { return m_canTouchFocus; }
    QVariantList isoOptions() const override { return m_isoOptions; }
    QVariantList shutterOptions() const override { return m_shutterOptions; }
    QVariantList apertureOptions() const override { return m_apertureOptions; }
    QVariantList whiteBalanceOptions() const override
    {
        return m_whiteBalanceOptions;
    }
    QVariantList focusModeOptions() const override { return m_focusModeOptions; }
    qulonglong isoValue() const override { return m_isoValue; }
    qulonglong shutterValue() const override { return m_shutterValue; }
    qulonglong apertureValue() const override { return m_apertureValue; }
    qulonglong whiteBalanceValue() const override { return m_whiteBalanceValue; }
    qulonglong focusModeValue() const override { return m_focusModeValue; }
    QString lastError() const override { return m_lastError; }

public slots:
    void reconnect() override;
    void setRecording(bool recording) override;
    void setIso(qulonglong value) override;
    void setShutterSpeed(qulonglong value) override;
    void setAperture(qulonglong value) override;
    void setWhiteBalance(qulonglong value) override;
    void setFocusMode(qulonglong value) override;
    void setAutoFocus(bool pressed) override;
    void touchFocus(qreal normalizedX, qreal normalizedY) override;

private slots:
    void applyStatus(int state, const QString &text, const QString &error);
    void applySnapshot(const QVariantMap &snapshot);
    void applyCommandError(const QString &error);

private:
    void clearCameraState();

    QThread *m_thread = nullptr;
    SonyCrSdkWorker *m_worker = nullptr;
    ConnectionState m_state = Disabled;
    QString m_connectionText;
    QString m_modelName = QStringLiteral("--");
    QString m_firmwareVersion = QStringLiteral("--");
    bool m_connected = false;
    bool m_recording = false;
    bool m_canRecord = false;
    bool m_canControlExposure = false;
    bool m_canControlFocus = false;
    bool m_canControlWhiteBalance = false;
    bool m_canAutoFocus = false;
    bool m_canTouchFocus = false;
    QVariantList m_isoOptions;
    QVariantList m_shutterOptions;
    QVariantList m_apertureOptions;
    QVariantList m_whiteBalanceOptions;
    QVariantList m_focusModeOptions;
    qulonglong m_isoValue = 0;
    qulonglong m_shutterValue = 0;
    qulonglong m_apertureValue = 0;
    qulonglong m_whiteBalanceValue = 0;
    qulonglong m_focusModeValue = 0;
    QString m_lastError;
};

#endif
