TEMPLATE = app
TARGET = pro1x-camera
QT += widgets maemo5
CONFIG += c++17 link_pkgconfig
PKGCONFIG += gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0

SOURCES += main.cpp camerawindow.cpp
HEADERS += camerawindow.h

target.path = /usr/bin
desktop.files = pro1x-camera.desktop
desktop.path = /usr/share/applications/hildon
INSTALLS += target desktop
