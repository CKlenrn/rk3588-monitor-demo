#include "cameracontrolbackend.h"
#include "hdmirxcontroller.h"
#include "lowlatencyvideopipelinecontroller.h"
#include "monitoranalysisoverlay.h"
#ifdef MONITOR_DEMO_HAVE_SONY_CRSDK
#include "sonycrsdkbackend.h"
#endif

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QSurfaceFormat>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {

int signalPipe[2] = {-1, -1};

void quitSignalHandler(int signalNumber)
{
    const char value = static_cast<char>(signalNumber);
    const ssize_t ignored = ::write(signalPipe[1], &value, sizeof(value));
    Q_UNUSED(ignored)
}

bool installSignalHandlers()
{
    if (::pipe(signalPipe) != 0)
        return false;

    for (int fd : signalPipe) {
        const int statusFlags = ::fcntl(fd, F_GETFL, 0);
        const int descriptorFlags = ::fcntl(fd, F_GETFD, 0);
        if (statusFlags < 0 || descriptorFlags < 0
                || ::fcntl(fd, F_SETFL, statusFlags | O_NONBLOCK) < 0
                || ::fcntl(fd, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
            ::close(signalPipe[0]);
            ::close(signalPipe[1]);
            signalPipe[0] = signalPipe[1] = -1;
            return false;
        }
    }

    struct sigaction action = {};
    action.sa_handler = quitSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGINT, &action, nullptr) == 0
            && ::sigaction(SIGTERM, &action, nullptr) == 0) {
        return true;
    }

    ::close(signalPipe[0]);
    ::close(signalPipe[1]);
    signalPipe[0] = signalPipe[1] = -1;
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QQuickWindow::setDefaultAlphaBuffer(true);
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("monitor_demo"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("监视器demo"));
    qmlRegisterType<MonitorAnalysisOverlay>("MonitorDemo", 1, 0,
                                            "MonitorAnalysisOverlay");

    QSocketNotifier *signalNotifier = nullptr;
    if (installSignalHandlers()) {
        signalNotifier = new QSocketNotifier(signalPipe[0],
                                             QSocketNotifier::Read, &app);
        QObject::connect(signalNotifier, &QSocketNotifier::activated,
                         &app, [&app]() {
            char buffer[16];
            while (::read(signalPipe[0], buffer, sizeof(buffer)) > 0) {}
            app.quit();
        });
    }

    HdmiRxController hdmi;
    LowLatencyVideoPipelineController video(&hdmi);
#ifdef MONITOR_DEMO_HAVE_SONY_CRSDK
    SonyCrSdkBackend camera;
#else
    UnavailableCameraControlBackend camera;
#endif

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("hdmiController"),
                                             &hdmi);
    engine.rootContext()->setContextProperty(QStringLiteral("videoController"),
                                             &video);
    engine.rootContext()->setContextProperty(QStringLiteral("cameraController"),
                                             &camera);
    engine.load(QUrl(QStringLiteral("qrc:/qml/main.qml")));
    if (engine.rootObjects().isEmpty())
        return 2;

    hdmi.start();
    const int result = app.exec();

    video.stop();
    hdmi.stop();
    if (signalPipe[0] >= 0)
        ::close(signalPipe[0]);
    if (signalPipe[1] >= 0)
        ::close(signalPipe[1]);
    return result;
}
