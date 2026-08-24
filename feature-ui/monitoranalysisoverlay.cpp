#include "monitoranalysisoverlay.h"

#include "lowlatencyvideopipelinecontroller.h"

#include <QPainter>
#include <QQuickWindow>

MonitorAnalysisOverlay::MonitorAnalysisOverlay(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(false);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

void MonitorAnalysisOverlay::setController(QObject *controller)
{
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (auto *video = qobject_cast<LowLatencyVideoPipelineController *>(controller)) {
        connect(video, &LowLatencyVideoPipelineController::analysisFrameReady,
                this, &MonitorAnalysisOverlay::updateAnalysisFrame);
    }
    emit controllerChanged();
}

bool MonitorAnalysisOverlay::bitAt(const QByteArray &bitmap, int bit)
{
    const int byte = bit >> 3;
    return byte >= 0 && byte < bitmap.size()
            && (static_cast<unsigned char>(bitmap.at(byte))
                & (1u << (bit & 7))) != 0;
}

void MonitorAnalysisOverlay::updateAnalysisFrame(
        int width, int height, const QByteArray &zebra,
        const QByteArray &peaking, const QByteArray &waveform)
{
    const bool dimensionsOk = width > 0 && height > 0
            && width <= 512 && height <= 512;
    const int expectedBytes = dimensionsOk ? (width * height + 7) / 8 : 0;
    if (!dimensionsOk
            || zebra.size() != expectedBytes
            || peaking.size() != expectedBytes
            || waveform.size() != expectedBytes) {
        m_overlay = QImage();
        m_waveform = QImage();
        m_overlayVisible = false;
        m_waveformVisible = false;
        update();
        if (window())
            window()->update();
        return;
    }

    if (m_overlay.size() != QSize(width, height))
        m_overlay = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    if (m_waveform.size() != QSize(width, height))
        m_waveform = QImage(width, height, QImage::Format_ARGB32_Premultiplied);
    m_overlay.fill(Qt::transparent);
    m_waveform.fill(Qt::transparent);
    m_overlayVisible = false;
    m_waveformVisible = false;

    const QRgb zebraColor = qRgba(255, 207, 51, 150);
    const QRgb peakingColor = qRgba(255, 52, 64, 230);
    const QRgb waveformColor = qRgba(90, 232, 132, 220);
    for (int y = 0; y < height; ++y) {
        QRgb *overlayLine = reinterpret_cast<QRgb *>(m_overlay.scanLine(y));
        QRgb *waveformLine = reinterpret_cast<QRgb *>(m_waveform.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int bit = y * width + x;
            if (bitAt(zebra, bit)) {
                overlayLine[x] = zebraColor;
                m_overlayVisible = true;
            }
            if (bitAt(peaking, bit)) {
                overlayLine[x] = peakingColor;
                m_overlayVisible = true;
            }
            if (bitAt(waveform, bit)) {
                waveformLine[x] = waveformColor;
                m_waveformVisible = true;
            }
        }
    }
    update();
    if (window())
        window()->update();
}

void MonitorAnalysisOverlay::paint(QPainter *painter)
{
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    if (m_overlayVisible && !m_overlay.isNull())
        painter->drawImage(boundingRect(), m_overlay);

    if (!m_waveformVisible || m_waveform.isNull())
        return;

    const qreal graphWidth = qMin<qreal>(520, width() * 0.32);
    const qreal graphHeight = graphWidth * 0.5;
    const QRectF panel(26, height() - graphHeight - 26,
                       graphWidth, graphHeight);
    painter->fillRect(panel, QColor(17, 20, 23, 220));
    painter->setPen(QColor(255, 255, 255, 38));
    for (int line = 1; line < 4; ++line) {
        const qreal y = panel.top() + panel.height() * line / 4;
        painter->drawLine(QPointF(panel.left(), y), QPointF(panel.right(), y));
    }
    painter->drawImage(panel.adjusted(8, 8, -8, -8), m_waveform);
    painter->setPen(QColor(255, 255, 255, 90));
    painter->drawRect(panel);
}
