#ifndef STREAM_RECEIVER_H
#define STREAM_RECEIVER_H

#include <QObject>
#include <QImage>
#include <QThread>
#include <QAtomicInt>

#ifdef USE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#endif

/**
 * @brief RTSP stream receiver / decoder
 *
 * FFmpeg decode flow:
 *   1. avformat_open_input() - open the stream (RTSP)
 *   2. avformat_find_stream_info() - probe stream info
 *   3. av_find_best_stream() - locate the video stream
 *   4. avcodec_alloc_context3() + avcodec_parameters_to_context() - set up the decoder
 *   5. avcodec_open2() - open the decoder
 *   6. av_read_frame() -> avcodec_send_packet() -> avcodec_receive_frame()
 *   7. sws_scale() YUV420P -> RGB24 -> QImage
 */
class StreamReceiver : public QThread
{
    Q_OBJECT

public:
    explicit StreamReceiver(QObject *parent = nullptr);
    ~StreamReceiver();

    bool open(const QString &url);       ///< open stream (rtmp:// or file path)
    void close();                        ///< close stream
    bool isOpen() const;                 ///< is open

signals:
    void frameReady(const QImage &frame);  ///< a decoded frame, sent to UI for display
    void errorOccurred(const QString &err);

protected:
    void run() override;  ///< thread main loop: read_frame -> decode -> emit

private:
#ifdef USE_FFMPEG
    AVFormatContext *m_fmtCtx;
    AVCodecContext  *m_videoCodecCtx;
    SwsContext      *m_swsCtx;
    int             m_videoStreamIdx;
    AVFrame         *m_frame;
    AVPacket        *m_pkt;
#endif
    QString          m_url;
    QAtomicInt       m_running;
    int              m_width;
    int              m_height;

    QImage convertFrameToQImage();  ///< YUV420P -> RGB24 -> QImage
};

#endif // STREAM_RECEIVER_H
