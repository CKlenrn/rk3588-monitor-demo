QT += core gui qml quick

CONFIG += c++17 link_pkgconfig warn_on
PKGCONFIG += gstreamer-1.0 gstreamer-video-1.0

TARGET = monitor_demo
TEMPLATE = app

SOURCES += \
    main.cpp \
    hdmirxcontroller.cpp \
    videopipelinecontroller.cpp \
    lowlatencyvideopipelinecontroller.cpp \
    monitoranalysisoverlay.cpp \
    cameracontrolbackend.cpp

HEADERS += \
    videotypes.h \
    hdmirxcontroller.h \
    videopipelinecontroller.h \
    lowlatencyvideopipelinecontroller.h \
    monitoranalysisoverlay.h \
    cameracontrolbackend.h

RESOURCES += qml.qrc

SONY_CRSDK_ROOT = $$(SONY_CRSDK_ROOT)
!isEmpty(SONY_CRSDK_ROOT) {
    SONY_CRSDK_INCLUDE = $$SONY_CRSDK_ROOT/app/CRSDK
    SONY_CRSDK_LIB = $$SONY_CRSDK_ROOT/external/crsdk
    !exists($$SONY_CRSDK_INCLUDE/CameraRemote_SDK.h) {
        error("SONY_CRSDK_ROOT does not contain app/CRSDK/CameraRemote_SDK.h")
    }
    !exists($$SONY_CRSDK_LIB/libCr_Core.so) {
        error("SONY_CRSDK_ROOT does not contain external/crsdk/libCr_Core.so")
    }

    DEFINES += MONITOR_DEMO_HAVE_SONY_CRSDK=1
    INCLUDEPATH += $$SONY_CRSDK_INCLUDE
    QMAKE_CXXFLAGS += -fsigned-char
    LIBS += -L$$SONY_CRSDK_LIB -lCr_Core
    QMAKE_RPATHDIR += $ORIGIN
    SOURCES += sonycrsdkbackend.cpp
    HEADERS += sonycrsdkbackend.h
    message("Sony Camera Remote SDK enabled from Git-external path")
} else {
    message("Sony Camera Remote SDK disabled; unavailable backend will be built")
}
