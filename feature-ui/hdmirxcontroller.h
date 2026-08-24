#ifndef HDMIRXCONTROLLER_H
#define HDMIRXCONTROLLER_H

#include "videotypes.h"

#include <QObject>
#include <QTimer>

class QSocketNotifier;

class HdmiRxController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    Q_PROPERTY(bool signalPresent READ signalPresent NOTIFY signalPresentChanged)
    Q_PROPERTY(int width READ width NOTIFY formatPropertiesChanged)
    Q_PROPERTY(int height READ height NOTIFY formatPropertiesChanged)
    Q_PROPERTY(QString frameRateText READ frameRateText NOTIFY formatPropertiesChanged)
    Q_PROPERTY(QString pixelFormat READ pixelFormat NOTIFY formatPropertiesChanged)
    Q_PROPERTY(QString colorimetry READ colorimetry NOTIFY formatPropertiesChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum State {
        Boot,
        NoSignal,
        Probing,
        Ready,
        Error
    };
    Q_ENUM(State)

    explicit HdmiRxController(QObject *parent = nullptr);
    ~HdmiRxController() override;

    State state() const { return m_state; }
    QString stateText() const;
    bool signalPresent() const { return m_signalPresent; }
    int width() const { return m_currentFormat.width; }
    int height() const { return m_currentFormat.height; }
    QString frameRateText() const;
    QString pixelFormat() const { return m_currentFormat.driverFormat; }
    QString colorimetry() const { return m_currentFormat.colorimetry; }
    QString lastError() const { return m_lastError; }
    VideoFormat currentFormat() const { return m_currentFormat; }

public slots:
    void start();
    void stop();
    void probeNow();

public:
    void pauseDeviceMonitoring();
    void resumeDeviceMonitoring();
    bool setLowLatencyEnabled(bool enabled);

signals:
    void stateChanged();
    void signalPresentChanged();
    void formatPropertiesChanged();
    void lastErrorChanged();
    void formatChanged(const VideoFormat &format);
    void signalLost();

private slots:
    void pollDevice();
    void processDeviceEvents();

private:
    enum QueryResult {
        FormatReady,
        SignalUnavailable,
        QueryFailed
    };

    bool ensureSafetySettings(bool *edidChanged = nullptr);
    bool ensureTextValue(const QString &path, const QByteArray &expected,
                         const QByteArray &replacement, bool *changed = nullptr);
    bool openDevice();
    void closeDevice();
    QueryResult queryFormat(VideoFormat *format, QString *errorText);
    void acceptCandidate(const VideoFormat &format);
    void markNoSignal();
    void markError(const QString &errorText);
    void setState(State state);
    void setLastError(const QString &errorText);

    State m_state = Boot;
    bool m_signalPresent = false;
    bool m_running = false;
    bool m_deviceMonitoringPaused = false;
    bool m_safetyReady = false;
    int m_fd = -1;
    QSocketNotifier *m_eventNotifier = nullptr;
    QTimer m_probeTimer;
    VideoFormat m_currentFormat;
    VideoFormat m_candidateFormat;
    int m_candidateSamples = 0;
    QString m_lastError;
};

#endif
