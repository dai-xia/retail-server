#include "monitor_widget.h"
#include "core/businessmanager.h"
#include <QDebug>

MonitorWidget::MonitorWidget(QWidget *parent)
    : QWidget(parent)
    , m_receiver(nullptr)
    , m_currentFd(-1)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // ---- Top: client list + control buttons ----
    QHBoxLayout *controlLayout = new QHBoxLayout();

    m_clientList = new QListWidget();
    m_clientList->setMaximumWidth(250);
    controlLayout->addWidget(m_clientList);

    QVBoxLayout *btnLayout = new QVBoxLayout();

    m_rtspUrlEdit = new QLineEdit("rtsp://127.0.0.1:8554/");
    m_rtspUrlEdit->setPlaceholderText("RTSP推流地址");
    btnLayout->addWidget(new QLabel("RTSP服务器(MediaMTX):"));
    btnLayout->addWidget(m_rtspUrlEdit);

    m_btnRefresh = new QPushButton("刷新客户端");
    m_btnStart   = new QPushButton("开始监控");
    m_btnStop    = new QPushButton("停止监控");
    m_btnStop->setEnabled(false);

    m_statusLabel = new QLabel("就绪");

    btnLayout->addWidget(m_btnRefresh);
    btnLayout->addWidget(m_btnStart);
    btnLayout->addWidget(m_btnStop);
    btnLayout->addWidget(m_statusLabel);
    btnLayout->addStretch();

    controlLayout->addLayout(btnLayout);
    mainLayout->addLayout(controlLayout);

    // ---- Bottom: video display area ----
    m_videoPlayer = new VideoPlayerWidget();
    m_videoPlayer->setMinimumSize(640, 480);
    mainLayout->addWidget(m_videoPlayer, 1);

    // ---- Signal connections ----
    connect(m_btnRefresh, &QPushButton::clicked, this, &MonitorWidget::refreshClientList);
    connect(m_btnStart,   &QPushButton::clicked, this, &MonitorWidget::onStartMonitorClicked);
    connect(m_btnStop,    &QPushButton::clicked, this, &MonitorWidget::onStopMonitorClicked);

    refreshClientList();
}

MonitorWidget::~MonitorWidget()
{
    if (m_receiver) {
        m_receiver->close();
        m_receiver->deleteLater();
    }
}

void MonitorWidget::refreshClientList()
{
    m_clientList->clear();
    auto clients = BusinessManager::getInstance()->getConnectedClients();
    for (const auto &client : clients) {
        int fd = client.first;
        QString id = client.second;
        m_clientList->addItem(QString("fd=%1 | %2").arg(fd).arg(id));
    }
    m_statusLabel->setText(QString("已连接客户端: %1").arg(clients.size()));
}

void MonitorWidget::onStartMonitorClicked()
{
    int row = m_clientList->currentRow();
    if (row < 0) {
        m_statusLabel->setText("请先选择客户端");
        return;
    }

    auto clients = BusinessManager::getInstance()->getConnectedClients();
    if (row >= clients.size()) return;

    m_currentFd = clients[row].first;
    QString clientId = clients[row].second;

    // Build RTSP stream URL: rtsp://server:8554/clientId
    QString rtspBase = m_rtspUrlEdit->text();
    if (!rtspBase.endsWith("/")) rtspBase += "/";
    QString rtspUrl = rtspBase + clientId;

    // 1. Send monitor_start command to the client
    BusinessManager::getInstance()->monitorStart(m_currentFd, rtspUrl);

    // 2. Start local StreamReceiver (delayed start, wait for client to push stream)
    if (m_receiver) {
        m_receiver->close();
        m_receiver->deleteLater();
    }

    m_receiver = new StreamReceiver(this);
    connect(m_receiver, &StreamReceiver::frameReady,
            m_videoPlayer, &VideoPlayerWidget::displayFrame);
    connect(m_receiver, &StreamReceiver::errorOccurred,
            this, &MonitorWidget::onStreamError);

    // RTSP pull URL matches push URL (MediaMTX uses the same URL)
    if (m_receiver->open(rtspUrl)) {
        m_receiver->start();
        m_statusLabel->setText(QString("监控中: %1").arg(clientId));
        m_btnStart->setEnabled(false);
        m_btnStop->setEnabled(true);
    } else {
        m_statusLabel->setText("拉流连接失败, 等待客户端推流后重试");
    }
}

void MonitorWidget::onStopMonitorClicked()
{
    if (m_currentFd >= 0) {
        BusinessManager::getInstance()->monitorStop(m_currentFd);
    }

    if (m_receiver) {
        m_receiver->close();
        m_receiver->deleteLater();
        m_receiver = nullptr;
    }

    m_currentFd = -1;
    m_btnStart->setEnabled(true);
    m_btnStop->setEnabled(false);
    m_statusLabel->setText("监控已停止");
}

void MonitorWidget::onFrameReady(const QImage &frame)
{
    Q_UNUSED(frame);
    // Frames are delivered directly to VideoPlayerWidget via signal
}

void MonitorWidget::onStreamError(const QString &err)
{
    m_statusLabel->setText(QString("流错误: %1").arg(err));
    qDebug() << "[MonitorWidget] stream error:" << err;
}

void MonitorWidget::autoStartMonitor()
{
    if (m_clientList->count() == 0) {
        m_statusLabel->setText("无客户端, 无法自动监控");
        return;
    }
    if (m_currentFd >= 0) {
        return;
    }
    m_clientList->setCurrentRow(0);
    onStartMonitorClicked();
}
