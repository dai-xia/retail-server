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
#include <libswresample/swresample.h>
#include <alsa/asoundlib.h>
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
 *   8. AAC decode -> swr_convert() FLTP -> S16LE -> ALSA playback
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
    QImage convertFrameToQImage();  ///< YUV420P -> RGB24 -> QImage
    bool openAudio();               ///< find + open audio stream decoder, init ALSA
    void closeAudio();              ///< close audio decoder + ALSA
    void playAudioFrame();          ///< decode FLTP -> S16LE -> ALSA write

#ifdef USE_FFMPEG
    AVFormatContext *m_fmtCtx;
    AVCodecContext  *m_videoCodecCtx;
    SwsContext      *m_swsCtx;
    int             m_videoStreamIdx;
    AVFrame         *m_frame;
    AVPacket        *m_pkt;

    AVCodecContext  *m_audioCodecCtx;
    SwrContext      *m_swrCtx;
    int             m_audioStreamIdx;
    AVFrame         *m_audioFrame;
    snd_pcm_t       *m_pcm;
#endif
    QString          m_url;
    QAtomicInt       m_running;
    int              m_width;
    int              m_height;
};

#endif // STREAM_RECEIVER_H
