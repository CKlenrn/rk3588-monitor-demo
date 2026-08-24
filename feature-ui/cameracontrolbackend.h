#ifndef CAMERACONTROLBACKEND_H
#define CAMERACONTROLBACKEND_H

#include <QObject>
#include <QString>
#include <QVariantList>

class ICameraControlBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(ConnectionState connectionState READ connectionState
               NOTIFY connectionStateChanged)
    Q_PROPERTY(QString connectionText READ connectionText
               NOTIFY connectionStateChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY cameraInfoChanged)
    Q_PROPERTY(QString firmwareVersion READ firmwareVersion
               NOTIFY cameraInfoChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectionStateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
    Q_PROPERTY(bool canRecord READ canRecord NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canControlExposure READ canControlExposure
               NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canControlFocus READ canControlFocus
               NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canControlWhiteBalance READ canControlWhiteBalance
               NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canAutoFocus READ canAutoFocus NOTIFY capabilitiesChanged)
    Q_PROPERTY(bool canTouchFocus READ canTouchFocus NOTIFY capabilitiesChanged)
    Q_PROPERTY(QVariantList isoOptions READ isoOptions NOTIFY cameraSettingsChanged)
    Q_PROPERTY(QVariantList shutterOptions READ shutterOptions
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(QVariantList apertureOptions READ apertureOptions
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(QVariantList whiteBalanceOptions READ whiteBalanceOptions
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(QVariantList focusModeOptions READ focusModeOptions
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(qulonglong isoValue READ isoValue NOTIFY cameraSettingsChanged)
    Q_PROPERTY(qulonglong shutterValue READ shutterValue
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(qulonglong apertureValue READ apertureValue
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(qulonglong whiteBalanceValue READ whiteBalanceValue
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(qulonglong focusModeValue READ focusModeValue
               NOTIFY cameraSettingsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum ConnectionState {
        Disabled,
        Disconnected,
        Discovering,
        Connecting,
        Ready,
        Recovering,
        CameraError
    };
    Q_ENUM(ConnectionState)

    explicit ICameraControlBackend(QObject *parent = nullptr)
        : QObject(parent) {}
    ~ICameraControlBackend() override = default;

    virtual ConnectionState connectionState() const = 0;
    virtual QString connectionText() const = 0;
    virtual QString modelName() const = 0;
    virtual QString firmwareVersion() const = 0;
    virtual bool connected() const = 0;
    virtual bool recording() const = 0;
    virtual bool canRecord() const = 0;
    virtual bool canControlExposure() const = 0;
    virtual bool canControlFocus() const = 0;
    virtual bool canControlWhiteBalance() const = 0;
    virtual bool canAutoFocus() const = 0;
    virtual bool canTouchFocus() const = 0;
    virtual QVariantList isoOptions() const = 0;
    virtual QVariantList shutterOptions() const = 0;
    virtual QVariantList apertureOptions() const = 0;
    virtual QVariantList whiteBalanceOptions() const = 0;
    virtual QVariantList focusModeOptions() const = 0;
    virtual qulonglong isoValue() const = 0;
    virtual qulonglong shutterValue() const = 0;
    virtual qulonglong apertureValue() const = 0;
    virtual qulonglong whiteBalanceValue() const = 0;
    virtual qulonglong focusModeValue() const = 0;
    virtual QString lastError() const = 0;

public slots:
    virtual void reconnect() = 0;
    virtual void setRecording(bool recording) = 0;
    virtual void setIso(qulonglong value) = 0;
    virtual void setShutterSpeed(qulonglong value) = 0;
    virtual void setAperture(qulonglong value) = 0;
    virtual void setWhiteBalance(qulonglong value) = 0;
    virtual void setFocusMode(qulonglong value) = 0;
    virtual void setAutoFocus(bool pressed) = 0;
    virtual void touchFocus(qreal normalizedX, qreal normalizedY) = 0;

signals:
    void connectionStateChanged();
    void cameraInfoChanged();
    void recordingChanged();
    void capabilitiesChanged();
    void cameraSettingsChanged();
    void lastErrorChanged();
};

class UnavailableCameraControlBackend final : public ICameraControlBackend
{
    Q_OBJECT

public:
    explicit UnavailableCameraControlBackend(QObject *parent = nullptr);

    ConnectionState connectionState() const override { return Disabled; }
    QString connectionText() const override;
    QString modelName() const override { return QStringLiteral("--"); }
    QString firmwareVersion() const override { return QStringLiteral("--"); }
    bool connected() const override { return false; }
    bool recording() const override { return false; }
    bool canRecord() const override { return false; }
    bool canControlExposure() const override { return false; }
    bool canControlFocus() const override { return false; }
    bool canControlWhiteBalance() const override { return false; }
    bool canAutoFocus() const override { return false; }
    bool canTouchFocus() const override { return false; }
    QVariantList isoOptions() const override { return {}; }
    QVariantList shutterOptions() const override { return {}; }
    QVariantList apertureOptions() const override { return {}; }
    QVariantList whiteBalanceOptions() const override { return {}; }
    QVariantList focusModeOptions() const override { return {}; }
    qulonglong isoValue() const override { return 0; }
    qulonglong shutterValue() const override { return 0; }
    qulonglong apertureValue() const override { return 0; }
    qulonglong whiteBalanceValue() const override { return 0; }
    qulonglong focusModeValue() const override { return 0; }
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

private:
    void rejectUnavailable();

    QString m_lastError;
};

#endif
