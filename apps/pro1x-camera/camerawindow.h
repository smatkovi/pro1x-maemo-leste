/*
 * pro1x-camera -- a Fremantle-style camera for Maemo Leste on the F(x)tec Pro1X.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CAMERAWINDOW_H
#define CAMERAWINDOW_H

#include <QAbstractButton>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;
typedef struct _GstMessage GstMessage;

class QToolButton;

/* One camera as libcamera names it, with what the app needs to know about it. */
struct CameraInfo {
    QString name;      /* libcamera id, e.g. /base/soc@0/cci@.../camera@10 */
    QString label;
    int stillWidth;
    int stillHeight;
    int rotation;      /* clockwise degrees the image needs in the landscape UI */
    bool mirrorPreview;
    bool hasLens;      /* voice coil motor for focus (rear only) */
};

/* Native window the video sink draws into. */
class Viewfinder : public QWidget
{
    Q_OBJECT
public:
    explicit Viewfinder(QWidget *parent = nullptr);
    QPaintEngine *paintEngine() const override { return nullptr; }
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
signals:
    void tapped();
};

/* The round on-screen shutter. */
class ShutterButton : public QAbstractButton
{
    Q_OBJECT
public:
    explicit ShutterButton(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(150, 150); }
protected:
    void paintEvent(QPaintEvent *event) override;
};

class CameraWindow : public QWidget
{
    Q_OBJECT
public:
    explicit CameraWindow(QWidget *parent = nullptr);
    ~CameraWindow() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void capture();
    void autofocus();
    void switchCamera();
    void openLastPicture();
    void captureFinished(const QString &path, const QString &error);
    void autofocusFinished(int position);
    void displayStatus(const QString &state);
    void updateRunning();

private:
    void startViewfinder();
    void stopViewfinder();
    static int busSyncHandler(GstBus *bus, GstMessage *message, void *self);
    QString sinkName() const;
    void updateGalleryButton();
    void setBusy(bool busy);
    bool lensAvailable() const;

    QList<CameraInfo> m_cameras;
    int m_current = 0;
    Viewfinder *m_viewfinder;
    ShutterButton *m_shutter;
    QToolButton *m_close;
    QToolButton *m_switch;
    QToolButton *m_gallery;
    GstElement *m_pipeline = nullptr;
    bool m_capturing = false;
    bool m_focusing = false;
    int m_lensFd = -1;        /* the lens only holds its position while open */
    int m_focus = 100;        /* driver default, roughly 1 m */
    QString m_lastPicture;
    bool m_displayOn = true;  /* mce's display_status_ind */
    bool m_paused = false;    /* the viewfinder stopped while not seen */
    QTimer *m_pauseTimer;
};

#endif
