#ifndef VIDEO_PLAYER_WIDGET_H
#define VIDEO_PLAYER_WIDGET_H

#include <QWidget>
#include <QImage>
#include <QPaintEvent>
#include <QMutex>

/**
 * @brief Widget that renders the live camera preview on the server side.
 * Receives a decoded QImage from StreamReceiver and draws it in paintEvent.
 */
class VideoPlayerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoPlayerWidget(QWidget *parent = nullptr);

public slots:
    void displayFrame(const QImage &frame);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage  m_currentFrame;
    QMutex  m_mutex;  /* Guards frame data between decode thread and UI thread */
};

#endif // VIDEO_PLAYER_WIDGET_H
