#ifndef MONITORANALYSISOVERLAY_H
#define MONITORANALYSISOVERLAY_H

#include <QByteArray>
#include <QImage>
#include <QQuickPaintedItem>

class MonitorAnalysisOverlay : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *controller READ controller WRITE setController
               NOTIFY controllerChanged)

public:
    explicit MonitorAnalysisOverlay(QQuickItem *parent = nullptr);

    QObject *controller() const { return m_controller; }
    void setController(QObject *controller);
    void paint(QPainter *painter) override;

signals:
    void controllerChanged();

private slots:
    void updateAnalysisFrame(int width, int height,
                             const QByteArray &zebra,
                             const QByteArray &peaking,
                             const QByteArray &waveform);

private:
    static bool bitAt(const QByteArray &bitmap, int bit);

    QObject *m_controller = nullptr;
    QImage m_overlay;
    QImage m_waveform;
    bool m_overlayVisible = false;
    bool m_waveformVisible = false;
};

#endif
