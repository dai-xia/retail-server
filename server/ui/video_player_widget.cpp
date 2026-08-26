#include "video_player_widget.h"
#include <QPainter>
#include <QMutexLocker>

VideoPlayerWidget::VideoPlayerWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
}

void VideoPlayerWidget::displayFrame(const QImage &frame)
{
    if (frame.isNull()) return;

    {
        QMutexLocker locker(&m_mutex);
        m_currentFrame = frame.copy();  /* Deep copy so the decode thread can release the source frame immediately */
    }
    update();  /* Trigger async paintEvent */
}

void VideoPlayerWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    QMutexLocker locker(&m_mutex);
    if (m_currentFrame.isNull()) return;

    QImage scaled = m_currentFrame.scaled(size(), Qt::KeepAspectRatio,
                                           Qt::FastTransformation);
    int x = (width() - scaled.width()) / 2;
    int y = (height() - scaled.height()) / 2;
    painter.drawImage(x, y, scaled);
}
