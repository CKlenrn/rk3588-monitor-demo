#ifndef VIDEOPIPELINECONTROLLER_H
#define VIDEOPIPELINECONTROLLER_H

#include "videotypes.h"

#include <QObject>
#include <QTimer>

#include <gst/gst.h>

class HdmiRxController;

class VideoPipelineController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY stateChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY droppedFramesChanged)
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(QString fillModeText READ fillModeText NOTIFY fillModeChanged)
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

    explicit VideoPipelineController(HdmiRxController *hdmi,
                                     QObject *parent = nullptr);
    ~VideoPipelineController() override;

    State state() const { return m_state; }
    QString stateText() const;
    bool streaming() const { return m_state == Streaming; }
    int droppedFrames() const { return m_droppedFrames; }
    FillMode fillMode() const { return m_fillMode; }
    QString fillModeText() const;
    QString lastError() const { return m_lastError; }

public slots:
    void stop();
    void setFillMode(FillMode mode);
    void toggleFillMode();

signals:
    void stateChanged();
    void droppedFramesChanged();
    void fillModeChanged();
    void lastErrorChanged();

private slots:
    void startForFormat(const VideoFormat &format);
    void handleSignalLost();
    void processBusMessages();
    void retryCurrentFormat();
    void incrementDroppedFrames();

private:
    static void queueOverrunCallback(GstElement *queue, gpointer userData);
    void teardownPipeline();
    void handlePipelineFailure(const QString &errorText);
    void applySinkProperties();
    void setState(State state);
    void setLastError(const QString &errorText);

    HdmiRxController *m_hdmi = nullptr;
    GstElement *m_pipeline = nullptr;
    GstElement *m_sink = nullptr;
    GstElement *m_queue = nullptr;
    GstBus *m_bus = nullptr;
    QTimer m_busTimer;
    QTimer m_retryTimer;
    State m_state = Stopped;
    FillMode m_fillMode = Fit;
    int m_droppedFrames = 0;
    VideoFormat m_currentFormat;
    QString m_lastError;
};

#endif
