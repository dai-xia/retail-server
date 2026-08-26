/**
 * @file stream_receiver.cpp
 * @brief RTSP stream receiver / decoder implementation
 *
 * Server-side: pulls the RTSP stream pushed by the client via MediaMTX, decodes
 * it and displays it on the UI.
 * Flow: avformat_open_input -> find_stream -> open_codec -> read_frame loop
 */

#include "stream_receiver.h"
#include <QDebug>

StreamReceiver::StreamReceiver(QObject *parent)
    : QThread(parent)
#ifdef USE_FFMPEG
    , m_fmtCtx(nullptr)
    , m_videoCodecCtx(nullptr)
    , m_swsCtx(nullptr)
    , m_videoStreamIdx(-1)
    , m_frame(nullptr)
    , m_pkt(nullptr)
#endif
    , m_running(0)
    , m_width(0)
    , m_height(0)
{
}

StreamReceiver::~StreamReceiver()
{
    close();
}

bool StreamReceiver::open(const QString &url)
{
#ifdef USE_FFMPEG
    m_url = url;

    /* RTSP pull options: TCP transport, short timeout, low latency */
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);  /* 5s timeout (microseconds) */
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_delay", "500000", 0);   /* max delay 500ms */

    int ret = avformat_open_input(&m_fmtCtx, url.toUtf8().constData(), NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        emit errorOccurred(QString("无法打开流: %1").arg(url));
        return false;
    }

    ret = avformat_find_stream_info(m_fmtCtx, NULL);
    if (ret < 0) {
        emit errorOccurred("探测流信息失败");
        close();
        return false;
    }

    m_videoStreamIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (m_videoStreamIdx < 0) {
        emit errorOccurred("未找到视频流");
        close();
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(
        m_fmtCtx->streams[m_videoStreamIdx]->codecpar->codec_id);
    if (!codec) {
        emit errorOccurred("找不到解码器");
        close();
        return false;
    }

    m_videoCodecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_videoCodecCtx,
                                   m_fmtCtx->streams[m_videoStreamIdx]->codecpar);
    m_videoCodecCtx->thread_count = 2;

    ret = avcodec_open2(m_videoCodecCtx, codec, NULL);
    if (ret < 0) {
        emit errorOccurred("解码器打开失败");
        close();
        return false;
    }

    m_width  = m_videoCodecCtx->width;
    m_height = m_videoCodecCtx->height;

    m_swsCtx = sws_getContext(m_width, m_height, m_videoCodecCtx->pix_fmt,
                               m_width, m_height, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, NULL, NULL, NULL);

    m_frame = av_frame_alloc();
    m_pkt   = av_packet_alloc();

    qDebug() << "[StreamReceiver] stream opened:" << url
             << m_width << "x" << m_height;
    return true;
#else
    emit errorOccurred("未编译FFmpeg支持");
    return false;
#endif
}

void StreamReceiver::close()
{
    m_running.store(0);
    wait(3000);

#ifdef USE_FFMPEG
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_frame)  { av_frame_free(&m_frame); m_frame = nullptr; }
    if (m_pkt)    { av_packet_free(&m_pkt); m_pkt = nullptr; }
    if (m_videoCodecCtx) { avcodec_free_context(&m_videoCodecCtx); m_videoCodecCtx = nullptr; }
    if (m_fmtCtx)  { avformat_close_input(&m_fmtCtx); m_fmtCtx = nullptr; }
#endif
    m_videoStreamIdx = -1;
}

bool StreamReceiver::isOpen() const
{
#ifdef USE_FFMPEG
    return m_fmtCtx != nullptr;
#else
    return false;
#endif
}

void StreamReceiver::run()
{
#ifdef USE_FFMPEG
    if (!m_fmtCtx || !m_videoCodecCtx) return;

    m_running.store(1);

    while (m_running.load()) {
        int ret = av_read_frame(m_fmtCtx, m_pkt);
        if (ret < 0) {
            /* stream end or network interruption */
            if (ret == AVERROR_EOF) {
                qDebug() << "[StreamReceiver] stream ended";
            } else {
                emit errorOccurred("读取帧失败");
            }
            break;
        }

        if (m_pkt->stream_index != m_videoStreamIdx) {
            av_packet_unref(m_pkt);
            continue;
        }

        ret = avcodec_send_packet(m_videoCodecCtx, m_pkt);
        av_packet_unref(m_pkt);

        if (ret < 0) continue;

        while (avcodec_receive_frame(m_videoCodecCtx, m_frame) == 0) {
            QImage img = convertFrameToQImage();
            if (!img.isNull()) {
                emit frameReady(img);
            }
        }
    }
#endif
    m_running.store(0);
}

QImage StreamReceiver::convertFrameToQImage()
{
#ifdef USE_FFMPEG
    if (!m_frame || !m_swsCtx) return QImage();

    QImage image(m_width, m_height, QImage::Format_RGB888);

    uint8_t *dstSlice[1] = { image.bits() };
    int dstLinesize[1] = { m_width * 3 };

    sws_scale(m_swsCtx,
              m_frame->data, m_frame->linesize,
              0, m_height,
              dstSlice, dstLinesize);

    return image;
#else
    return QImage();
#endif
}
