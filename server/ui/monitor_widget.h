#ifndef MONITOR_WIDGET_H
#define MONITOR_WIDGET_H

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "core/stream_receiver.h"
#include "video_player_widget.h"

/**
 * @brief Real-time server-side monitoring panel.
 *
 * Control signaling runs over TCP JSON (monitor_start/stop sent to the client),
 * while video data is delivered via RTSP (client -> MediaMTX -> server pull).
 * This separation keeps control and media paths independent.
 */
class MonitorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MonitorWidget(QWidget *parent = nullptr);
    ~MonitorWidget();

public slots:
    void refreshClientList();
    void onStartMonitorClicked();
    void onStopMonitorClicked();
    void autoStartMonitor();

private slots:
    void onFrameReady(const QImage &frame);
    void onStreamError(const QString &err);

private:
    QListWidget   *m_clientList;

    QLineEdit     *m_rtspUrlEdit;
    QPushButton   *m_btnStart;
    QPushButton   *m_btnStop;
    QPushButton   *m_btnRefresh;
    QLabel        *m_statusLabel;

    VideoPlayerWidget *m_videoPlayer;

    StreamReceiver *m_receiver;

    int m_currentFd;
};

#endif // MONITOR_WIDGET_H
