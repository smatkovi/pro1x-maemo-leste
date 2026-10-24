/*
 * pro1x-camera -- a Fremantle-style camera for Maemo Leste on the F(x)tec Pro1X.
 *
 * Viewfinder: libcamerasrc -> videoflip -> xvimagesink (or glimagesink) drawn
 * into a native child window. A still is taken by stopping the viewfinder --
 * libcamera hands a camera to one user only -- and running a second pipeline
 * at full sensor resolution into an appsink; after a few frames for exposure
 * and white balance to settle, the last one is saved as JPEG.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "camerawindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <QtMaemo5/qmaemo5informationbox.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>

static const int PreviewWidth = 1280;
static const int PreviewHeight = 960;   /* both sensors are 4:3 */
static const int SettleFrames = 8;      /* frames for AE/AWB before the still */

/* Fremantle icons ship as PNGs in hicolor/<size>/hildon. */
static QIcon hildonIcon(const QString &name)
{
    static const char *const sizes[] = { "scalable", "64x64", "48x48", "32x32" };
    QIcon icon;
    for (const char *size : sizes) {
        const QString dir = QStringLiteral("/usr/share/icons/hicolor/%1/hildon/").arg(QLatin1String(size));
        if (icon.isNull() && QFile::exists(dir + name + ".png"))
            icon.addFile(dir + name + ".png", QSize(), QIcon::Normal);
        if (QFile::exists(dir + name + "_pressed.png"))
            icon.addFile(dir + name + "_pressed.png", QSize(), QIcon::Active);
    }
    return icon;
}

/* videoflip method for a clockwise rotation, optionally mirrored. */
static QString flipMethod(int rotation, bool mirror)
{
    switch (((rotation % 360) + 360) % 360) {
    case 90:  return mirror ? "upper-left-diagonal" : "clockwise";
    case 180: return mirror ? "vertical-flip" : "rotate-180";
    case 270: return mirror ? "upper-right-diagonal" : "counterclockwise";
    default:  return mirror ? "horizontal-flip" : "none";
    }
}

static int envInt(const char *name, int fallback)
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue(name, &ok);
    return ok ? v : fallback;
}

/* --------------------------------------------------------------------- */

Viewfinder::Viewfinder(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void Viewfinder::mouseReleaseEvent(QMouseEvent *)
{
    emit tapped();
}

ShutterButton::ShutterButton(QWidget *parent)
    : QAbstractButton(parent)
{
    setFocusPolicy(Qt::NoFocus);
}

void ShutterButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int side = qMin(width(), height()) - 8;
    const QRectF ring((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    const QColor accent = isEnabled() ? QColor(0x1a, 0x7d, 0xe6) : QColor(0x55, 0x55, 0x55);

    p.setPen(QPen(isEnabled() ? Qt::white : Qt::gray, 6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ring);
    p.setPen(Qt::NoPen);
    p.setBrush(isDown() ? accent.darker(150) : accent);
    p.drawEllipse(ring.adjusted(14, 14, -14, -14));
}

/* --------------------------------------------------------------------- */

CameraWindow::CameraWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(tr("Camera"));
    setAttribute(Qt::WA_DeleteOnClose, false);

    /*
     * libcamera's Rotation property is 270 (rear) and 90 (front) for the
     * portrait panel; Hildon runs the panel rotated to landscape, which
     * leaves 180 and 0. PRO1X_CAMERA_ROTATION_BACK/FRONT override that.
     */
    m_cameras << CameraInfo{ "/base/soc@0/cci@5c1b000/i2c-bus@0/camera@10", tr("Back camera"),
                             3992, 3000, envInt("PRO1X_CAMERA_ROTATION_BACK", 180), false }
              << CameraInfo{ "/base/soc@0/cci@5c1b000/i2c-bus@1/camera@2d", tr("Front camera"),
                             3256, 2448, envInt("PRO1X_CAMERA_ROTATION_FRONT", 0), true };

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setAutoFillBackground(true);

    m_viewfinder = new Viewfinder(this);
    connect(m_viewfinder, &Viewfinder::tapped, this, &CameraWindow::capture);

    auto makeButton = [this](const QString &icon, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setIcon(hildonIcon(icon));
        b->setIconSize(QSize(96, 96));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        b->setStyleSheet("QToolButton { border: none; background: transparent; }");
        return b;
    };
    m_close = makeButton("camera_overlay_close", tr("Close"));
    m_switch = makeButton("camera_scene_mode_portrait", tr("Switch camera"));
    m_gallery = makeButton("camera_imageviewer_dimmed", tr("Last picture"));
    m_shutter = new ShutterButton(this);

    connect(m_close, &QToolButton::clicked, this, &QWidget::close);
    connect(m_switch, &QToolButton::clicked, this, &CameraWindow::switchCamera);
    connect(m_gallery, &QToolButton::clicked, this, &CameraWindow::openLastPicture);
    connect(m_shutter, &QAbstractButton::clicked, this, &CameraWindow::capture);

    auto *side = new QVBoxLayout;
    side->setContentsMargins(24, 24, 24, 24);
    side->addWidget(m_close, 0, Qt::AlignHCenter);
    side->addStretch();
    side->addWidget(m_switch, 0, Qt::AlignHCenter);
    side->addSpacing(32);
    side->addWidget(m_shutter, 0, Qt::AlignHCenter);
    side->addSpacing(32);
    side->addWidget(m_gallery, 0, Qt::AlignHCenter);
    side->addStretch();

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_viewfinder, 1);
    layout->addLayout(side);
}

CameraWindow::~CameraWindow()
{
    stopViewfinder();
}

QString CameraWindow::sinkName() const
{
    static QString sink;
    if (!sink.isEmpty())
        return sink;
    sink = qEnvironmentVariable("PRO1X_CAMERA_SINK");
    if (sink.isEmpty()) {
        /* xvimagesink only reaches READY if the X server offers an Xv port. */
        GstElement *xv = gst_element_factory_make("xvimagesink", nullptr);
        if (xv && gst_element_set_state(xv, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE)
            sink = "xvimagesink";
        else
            sink = "glimagesink";
        if (xv) {
            gst_element_set_state(xv, GST_STATE_NULL);
            gst_object_unref(xv);
        }
    }
    return sink;
}

int CameraWindow::busSyncHandler(GstBus *, GstMessage *message, void *data)
{
    auto *self = static_cast<CameraWindow *>(data);

    if (gst_is_video_overlay_prepare_window_handle_message(message)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(message)),
                                            (guintptr) self->m_viewfinder->winId());
        gst_message_unref(message);
        return GST_BUS_DROP;
    }
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *err = nullptr;
        gst_message_parse_error(message, &err, nullptr);
        qWarning("pro1x-camera: %s", err ? err->message : "pipeline error");
        g_clear_error(&err);
    }
    return GST_BUS_PASS;
}

void CameraWindow::startViewfinder()
{
    if (m_pipeline || m_capturing)
        return;
    const CameraInfo &cam = m_cameras.at(m_current);
    const QString desc = QStringLiteral(
        "libcamerasrc camera-name=\"%1\" ! video/x-raw,width=%2,height=%3 ! "
        "videoconvert ! videoflip method=%4 ! videoconvert ! %5 force-aspect-ratio=true")
        .arg(cam.name).arg(PreviewWidth).arg(PreviewHeight)
        .arg(flipMethod(cam.rotation, cam.mirrorPreview)).arg(sinkName());

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline) {
        QMaemo5InformationBox::information(this, tr("Cannot start the camera: %1")
                                           .arg(QString::fromUtf8(err ? err->message : "?")));
        g_clear_error(&err);
        return;
    }
    GstBus *bus = gst_element_get_bus(m_pipeline);
    gst_bus_set_sync_handler(bus, (GstBusSyncHandler) busSyncHandler, this, nullptr);
    gst_object_unref(bus);
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

void CameraWindow::stopViewfinder()
{
    if (!m_pipeline)
        return;
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
}

/* Runs in a worker thread: one full-resolution frame into *image. */
static QString grabStill(const CameraInfo &cam, QImage *image)
{
    const QString desc = QStringLiteral(
        "libcamerasrc camera-name=\"%1\" ! video/x-raw,width=%2,height=%3 ! "
        "videoconvert ! videoflip method=%4 ! videoconvert ! video/x-raw,format=RGB ! "
        "appsink name=snap sync=false max-buffers=1 drop=true")
        .arg(cam.name).arg(cam.stillWidth).arg(cam.stillHeight)
        .arg(flipMethod(cam.rotation, false));

    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!pipeline) {
        const QString msg = QString::fromUtf8(err ? err->message : "pipeline");
        g_clear_error(&err);
        return msg;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "snap");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GstSample *last = nullptr;
    for (int i = 0; i < SettleFrames; i++) {
        GstSample *s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 6 * GST_SECOND);
        if (!s)
            break;
        if (last)
            gst_sample_unref(last);
        last = s;
    }

    QString error;
    GstVideoInfo info;
    GstMapInfo map;
    if (!last) {
        error = QObject::tr("The camera delivered no picture");
    } else if (!gst_video_info_from_caps(&info, gst_sample_get_caps(last))) {
        error = QObject::tr("Unknown picture format");
    } else if (!gst_buffer_map(gst_sample_get_buffer(last), &map, GST_MAP_READ)) {
        error = QObject::tr("Cannot read the picture");
    } else {
        *image = QImage(map.data, GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                        GST_VIDEO_INFO_PLANE_STRIDE(&info, 0), QImage::Format_RGB888).copy();
        gst_buffer_unmap(gst_sample_get_buffer(last), &map);
    }
    if (last)
        gst_sample_unref(last);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return error;
}

void CameraWindow::capture()
{
    if (m_capturing)
        return;
    m_capturing = true;
    m_shutter->setEnabled(false);
    m_switch->setEnabled(false);
    stopViewfinder();
    QMaemo5InformationBox::information(this, tr("Taking picture..."), 2000);

    const CameraInfo cam = m_cameras.at(m_current);
    QThread *worker = QThread::create([this, cam]() {
        QImage image;
        QString error = grabStill(cam, &image);
        QString path;
        if (error.isEmpty()) {
            QDir dir(QDir::homePath() + "/MyDocs/DCIM");
            dir.mkpath(".");
            path = dir.filePath(QDateTime::currentDateTime().toString("'IMG_'yyyyMMdd_HHmmss'.jpg'"));
            if (!image.save(path, "JPEG", 92))
                error = tr("Cannot save %1").arg(path);
        }
        QMetaObject::invokeMethod(this, "captureFinished", Qt::QueuedConnection,
                                  Q_ARG(QString, error.isEmpty() ? path : QString()),
                                  Q_ARG(QString, error));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void CameraWindow::captureFinished(const QString &path, const QString &error)
{
    m_capturing = false;
    m_shutter->setEnabled(true);
    m_switch->setEnabled(true);
    if (error.isEmpty()) {
        m_lastPicture = path;
        updateGalleryButton();
        QMaemo5InformationBox::information(this, tr("Picture saved to %1").arg(QDir(path).dirName()));
    } else {
        QMaemo5InformationBox::information(this, error);
    }
    if (isVisible())
        startViewfinder();
}

void CameraWindow::updateGalleryButton()
{
    QImage thumb(m_lastPicture);
    if (thumb.isNull())
        return;
    m_gallery->setIcon(QIcon(QPixmap::fromImage(
        thumb.scaled(m_gallery->iconSize() * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
}

void CameraWindow::switchCamera()
{
    if (m_capturing)
        return;
    stopViewfinder();
    m_current = (m_current + 1) % m_cameras.size();
    m_switch->setIcon(hildonIcon(m_current == 0 ? "camera_scene_mode_portrait"
                                                : "camera_scene_mode_landscape"));
    QMaemo5InformationBox::information(this, m_cameras.at(m_current).label, 1500);
    startViewfinder();
}

void CameraWindow::openLastPicture()
{
    if (!m_lastPicture.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastPicture));
}

void CameraWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Camera:
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        capture();
        break;
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
        close();
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

void CameraWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    /* The native viewfinder window has to exist before the sink asks for it. */
    QTimer::singleShot(0, this, &CameraWindow::startViewfinder);
}

void CameraWindow::closeEvent(QCloseEvent *event)
{
    stopViewfinder();
    QWidget::closeEvent(event);
}
