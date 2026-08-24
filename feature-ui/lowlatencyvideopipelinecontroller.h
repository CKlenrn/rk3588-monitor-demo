#ifndef LOWLATENCYVIDEOPIPELINECONTROLLER_H
#define LOWLATENCYVIDEOPIPELINECONTROLLER_H

#include "videotypes.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QTimer>

class HdmiRxController;

class LowLatencyVideoPipelineController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY stateChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(QString fillModeText READ fillModeText NOTIFY fillModeChanged)
    Q_PROPERTY(bool falseColorEnabled READ falseColorEnabled WRITE setFalseColorEnabled
               NOTIFY imageAssistChanged)
    Q_PROPERTY(int zoomFactor READ zoomFactor WRITE setZoomFactor NOTIFY zoomFactorChanged)
    Q_PROPERTY(bool zebraEnabled READ zebraEnabled WRITE setZebraEnabled
               NOTIFY imageAssistChanged)
    Q_PROPERTY(int zebraLevel READ zebraLevel WRITE setZebraLevel
               NOTIFY imageAssistChanged)
    Q_PROPERTY(bool peakingEnabled READ peakingEnabled WRITE setPeakingEnabled
               NOTIFY imageAssistChanged)
    Q_PROPERTY(int peakingSensitivity READ peakingSensitivity WRITE setPeakingSensitivity
               NOTIFY imageAssistChanged)
    Q_PROPERTY(bool waveformEnabled READ waveformEnabled WRITE setWaveformEnabled
               NOTIFY imageAssistChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum State {
        Stopped,
        Starting,
        Streaming,
        Reconfiguring,
        PipelineError
    };
    Q_ENUM(State)

    enum FillMode {
        Fit,
        Fill
    };
    Q_ENUM(FillMode)

    explicit LowLatencyVideoPipelineController(HdmiRxController *hdmi,
                                               QObject *parent = nullptr);
    ~LowLatencyVideoPipelineController() override;

    State state() const { return m_state; }
    QString stateText() const;
    bool streaming() const { return m_state == Streaming; }
    int droppedFrames() const { return 0; }
    FillMode fillMode() const { return m_fillMode; }
    QString fillModeText() const;
    bool falseColorEnabled() const { return m_falseColorEnabled; }
    int zoomFactor() const { return m_zoomFactor; }
    bool zebraEnabled() const { return m_zebraEnabled; }
    int zebraLevel() const { return m_zebraLevel; }
    bool peakingEnabled() const { return m_peakingEnabled; }
    int peakingSensitivity() const { return m_peakingSensitivity; }
    bool waveformEnabled() const { return m_waveformEnabled; }
    QString lastError() const { return m_lastError; }

public slots:
    void stop();
    void setFillMode(FillMode mode);
    void toggleFillMode();
    void setFalseColorEnabled(bool enabled);
    void toggleFalseColor();
    void setZoomFactor(int factor);
    void cycleZoomFactor();
    void setZebraEnabled(bool enabled);
    void setZebraLevel(int level);
    void setPeakingEnabled(bool enabled);
    void setPeakingSensitivity(int sensitivity);
    void setWaveformEnabled(bool enabled);

signals:
    void stateChanged();
    void droppedFramesChanged();
    void fillModeChanged();
    void imageAssistChanged();
    void zoomFactorChanged();
    void analysisFrameReady(int width, int height,
                            const QByteArray &zebra,
                            const QByteArray &peaking,
                            const QByteArray &waveform);
    void lastErrorChanged();

private slots:
    void startForFormat(const VideoFormat &format);
    void handleSignalLost();
    void handleProcessStarted();
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);
    void drainStandardOutput();
    void drainStandardError();
    void retryCurrentFormat();

private:
    enum ProcessPhase {
        Idle,
        Probing,
        Playing
    };

    QString helperPath() const;
    QString outputErrorText(const QString &fallback) const;
    void startProbe();
    void startHelper();
    bool stopProcessAndRestore();
    void restartForSettings();
    void handleFailure(const QString &errorText);
    void setState(State state);
    void setLastError(const QString &errorText);
    void appendProcessText(const QByteArray &text);
    void parseAnalysisFrames();
    void clearAnalysisFrame();

    HdmiRxController *m_hdmi = nullptr;
    QProcess m_process;
    QTimer m_retryTimer;
    State m_state = Stopped;
    FillMode m_fillMode = Fit;
    bool m_falseColorEnabled = false;
    int m_zoomFactor = 1;
    bool m_zebraEnabled = false;
    int m_zebraLevel = 95;
    bool m_peakingEnabled = false;
    int m_peakingSensitivity = 50;
    bool m_waveformEnabled = false;
    ProcessPhase m_phase = Idle;
    bool m_stopping = false;
    bool m_useExplicitSync = true;
    VideoFormat m_currentFormat;
    QString m_v4l2Format;
    QString m_drmFormat;
    QByteArray m_processOutput;
    QByteArray m_stdoutBuffer;
    QString m_lastError;
};

#endif
