/*
 * pro1x-camera -- a Fremantle-style camera for Maemo Leste on the F(x)tec Pro1X.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "camerawindow.h"

#include <QApplication>
#include <gst/gst.h>

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);
    /*
     * Leste's Qt5 xcb plugin asserts on the first touch after mce has
     * blanked the screen (the touchscreen comes back as a new XI2 device it
     * never registers). Without XI2 touches arrive as plain mouse events.
     */
    qputenv("QT_XCB_NO_XI2", "1");
    QApplication app(argc, argv);
    app.setApplicationName("pro1x-camera");

    CameraWindow window;
    window.showFullScreen();
    return app.exec();
}
