#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "stylehelper.h"
#include <QDateTime>
#include <QMessageBox>
#include <QTimer>
#include <QDebug>
#include <QCoreApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_memberWidget(nullptr)
    , m_goodsWidget(nullptr)
    , m_orderWidget(nullptr)
    , m_clientWidget(nullptr)
    , m_monitorWidget(nullptr)
    , m_otaWidget(nullptr)
{
    ui->setupUi(this);
    this->setWindowTitle("无人零售系统服务端管理平台 (Epoll+线程池版)");
    this->resize(1500, 1000);

    applyModernStyle();

    initSystem();
}

void MainWindow::applyModernStyle()
{
    QString style;
    style += "QMainWindow {"
             "    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
             "        stop:0 #1a1a2e, stop:0.5 #16213e, stop:1 #0f3460);"
             "}";
    style += "QWidget { color: #ecf0f1; }";
    style += "QMenuBar {"
             "    background: #16213e;"
             "    border-bottom: 2px solid #e94560;"
             "    padding: 5px;"
             "}";
    style += "QMenuBar::item {"
             "    background: transparent;"
             "    padding: 8px 16px;"
             "    color: #ecf0f1;"
             "    font-weight: bold;"
             "}";
    style += "QMenuBar::item:selected {"
             "    background: #e94560;"
             "    border-radius: 4px;"
             "}";
    style += "QMenu {"
             "    background: #16213e;"
             "    border: 1px solid #e94560;"
             "    padding: 5px;"
             "}";
    style += "QMenu::item {"
             "    padding: 8px 20px;"
             "    color: #ecf0f1;"
             "}";
    style += "QMenu::item:selected {"
             "    background: #e94560;"
             "    border-radius: 4px;"
             "}";
    style += "QTextEdit {"
             "    background: #0f1419;"
             "    border: 2px solid #2d3a4a;"
             "    border-radius: 8px;"
             "    color: #00ff88;"
             "    font-family: 'Consolas', 'Monaco', monospace;"
             "    font-size: 30px;"
             "    padding: 10px;"
             "}";
    style += "QTextEdit QScrollBar:vertical {"
             "    background: #16213e;"
             "    width: 12px;"
             "    border-radius: 6px;"
             "}";
    style += "QTextEdit QScrollBar::handle:vertical {"
             "    background: #e94560;"
             "    border-radius: 6px;"
             "    min-height: 30px;"
             "}";
    style += "QLabel {"
             "    color: #ecf0f1;"
             "    font-size: 24px;"
             "    font-weight: bold;"
             "}";
    style += "QStatusBar {"
             "    background: #16213e;"
             "    color: #ecf0f1;"
             "    border-top: 2px solid #e94560;"
             "}";
    style += StyleHelper::commonButtonStyle();
    style += StyleHelper::commonTableStyle();
    style += StyleHelper::commonInputStyle();
    // Main window specific QComboBox style
    style += "QComboBox {"
             "    background: #0f1419;"
             "    border: 2px solid #2d3a4a;"
             "    border-radius: 6px;"
             "    padding: 8px;"
             "    color: #ecf0f1;"
             "    min-width: 120px;"
             "}";
    style += "QComboBox:focus {"
             "    border-color: #e94560;"
             "}";
    style += "QComboBox::drop-down {"
             "    border: none;"
             "    width: 30px;"
             "}";
    style += "QComboBox QAbstractItemView {"
             "    background: #16213e;"
             "    color: #ecf0f1;"
             "    selection-background-color: #e94560;"
             "    border: 1px solid #e94560;"
             "}";
    style += StyleHelper::commonGroupBoxStyle();
    // Main window specific QTabWidget / QTabBar style
    style += "QTabWidget::pane {"
             "    border: 2px solid #2d3a4a;"
             "    border-radius: 8px;"
             "    background: #0f1419;"
             "}";
    style += "QTabBar::tab {"
             "    background: #16213e;"
             "    border: 1px solid #2d3a4a;"
             "    border-bottom: none;"
             "    border-top-left-radius: 8px;"
             "    border-top-right-radius: 8px;"
             "    padding: 12px 24px;"
             "    margin-right: 3px;"
             "    font-weight: bold;"
             "    color: #95a5a6;"
             "}";
    style += "QTabBar::tab:selected {"
             "    background: #e94560;"
             "    color: white;"
             "    border-color: #e94560;"
             "}";
    style += "QTabBar::tab:hover:!selected {"
             "    background: #1f2d3d;"
             "    color: #ecf0f1;"
             "}";
    this->setStyleSheet(style);

    ui->textEdit_log->setStyleSheet(
        "QTextEdit {"
        "    background: #0a0f14;"
        "    border: 3px solid #e94560;"
        "    border-radius: 10px;"
        "    color: #00ff88;"
        "    font-family: 'Consolas', 'Monaco', 'Courier New', monospace;"
        "    font-size: 20px;"
        "    padding: 15px;"
        "    line-height: 1.5;"
        "}"
    );
}

MainWindow::~MainWindow()
{
    // Stop server first to avoid stopServer() emitting signals to deleted objects
    stopServer();

    // Disconnect signals to prevent emission during destruction
    disconnect(BusinessManager::getInstance(), nullptr, this, nullptr);

    if(m_memberWidget)
    {
        m_memberWidget->close();
        delete m_memberWidget;
        m_memberWidget = nullptr;
    }
    if(m_goodsWidget)
    {
        m_goodsWidget->close();
        delete m_goodsWidget;
        m_goodsWidget = nullptr;
    }
    if(m_orderWidget)
    {
        m_orderWidget->close();
        delete m_orderWidget;
        m_orderWidget = nullptr;
    }
    if(m_clientWidget)
    {
        m_clientWidget->close();
        delete m_clientWidget;
        m_clientWidget = nullptr;
    }
    if(m_monitorWidget)
    {
        m_monitorWidget->close();
        delete m_monitorWidget;
        m_monitorWidget = nullptr;
    }
    if(m_otaWidget)
    {
        m_otaWidget->close();
        delete m_otaWidget;
        m_otaWidget = nullptr;
    }

    delete ui;
}

void MainWindow::initSystem()
{
    // 1. Initialize database (delegated to BusinessManager)
    bool dbOk = BusinessManager::getInstance()->initDatabase();
    if(!dbOk)
    {
        QMessageBox::critical(this, "错误", "MySQL数据库初始化失败！请确认MySQL服务已启动。");
        return;
    }
    slotAddLog("MySQL数据库初始化成功(连接池模式)");

    // 1.5 Connect BusinessManager signals (Network -> Business)
    BusinessManager::getInstance()->initialize();

    // 2. Create sub-widgets as independent windows (no parent)
    m_memberWidget = new MemberWidget(nullptr);
    m_goodsWidget = new GoodsWidget(nullptr);
    m_orderWidget = new OrderWidget(nullptr);
    m_clientWidget = new ClientWidget(nullptr);
    m_monitorWidget = new MonitorWidget(nullptr);
    m_otaWidget = new OtaWidget(nullptr);

    m_memberWidget->setWindowTitle("会员管理");
    m_goodsWidget->setWindowTitle("商品库存管理");
    m_orderWidget->setWindowTitle("订单管理");
    m_clientWidget->setWindowTitle("客户端监控");
    m_otaWidget->setWindowTitle("OTA升级管理");

    // 3. Wire up signals (centralized in MainWindow)

    // === Network -> Business (receive path) ===
    // Already connected in BusinessManager::initialize()

    // === Business -> Network (send path) ===
    // Already connected in BusinessManager::initialize()

    // === Business -> UI (data change notifications) ===
    connect(BusinessManager::getInstance(), &BusinessManager::signalAddLog,
            this, &MainWindow::slotAddLog);
    connect(BusinessManager::getInstance(), &BusinessManager::signalMemberDataChanged,
            m_memberWidget, &MemberWidget::slotMemberDataChanged);
    connect(BusinessManager::getInstance(), &BusinessManager::signalGoodsDataChanged,
            m_goodsWidget, &GoodsWidget::slotGoodsDataChanged);
    connect(BusinessManager::getInstance(), &BusinessManager::signalOrderDataChanged,
            m_orderWidget, &OrderWidget::slotOrderDataChanged);
    connect(BusinessManager::getInstance(), &BusinessManager::signalClientListChanged,
            m_clientWidget, &ClientWidget::slotClientListChanged);
    connect(BusinessManager::getInstance(), &BusinessManager::signalClientListChanged,
            m_goodsWidget, &GoodsWidget::updateClientComboBox);
    /* On client list change: refresh monitor panel; auto-open monitor window and pull stream when clients exist */
    connect(BusinessManager::getInstance(), &BusinessManager::signalClientListChanged,
            m_monitorWidget, &MonitorWidget::refreshClientList);
    connect(BusinessManager::getInstance(), &BusinessManager::signalClientListChanged,
            this, [this]() {
                auto clients = BusinessManager::getInstance()->getConnectedClients();
                if (clients.isEmpty()) return;
                if (!m_monitorWidget->isVisible()) {
                    on_action_monitor_triggered();
                }
                QTimer::singleShot(1000, m_monitorWidget, &MonitorWidget::autoStartMonitor);
            });
    connect(BusinessManager::getInstance(), &BusinessManager::signalOtaDataChanged,
            m_otaWidget, &OtaWidget::slotOtaDataChanged);

    ui->textEdit_log->setReadOnly(true);

    // 5. Start network server (delegated to BusinessManager)
    startServer();
}

void MainWindow::startServer()
{
    qDebug() << "MainWindow::startServer called - using C-style epoll framework";
    bool ok = BusinessManager::getInstance()->startServer("0.0.0.0", 9090);
    if(ok)
    {
        slotAddLog("✅ 服务端启动成功 (C风格epoll框架)");
        qDebug() << "Server started successfully, running state:" << BusinessManager::getInstance()->isServerRunning();
    }
    else
    {
        QMessageBox::critical(this, "错误", "服务端启动失败！");
        slotAddLog("❌ 服务端启动失败");
        qDebug() << "Server start failed";
    }
}

void MainWindow::stopServer()
{
    BusinessManager::getInstance()->stopServer();
    slotAddLog(" 服务端已停止");
}

void MainWindow::slotAddLog(QString log)
{
    QString time = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    ui->textEdit_log->append(QString("[%1] %2").arg(time).arg(log));
    ui->textEdit_log->moveCursor(QTextCursor::End);
}

void MainWindow::on_action_member_triggered()
{
    m_memberWidget->resize(1200, 800);
    m_memberWidget->show();
    m_memberWidget->raise();
    m_memberWidget->activateWindow();
}

void MainWindow::on_action_goods_triggered()
{
    m_goodsWidget->resize(1200, 800);
    m_goodsWidget->show();
    m_goodsWidget->raise();
    m_goodsWidget->activateWindow();
}

void MainWindow::on_action_order_triggered()
{
    m_orderWidget->resize(1200, 800);
    m_orderWidget->show();
    m_orderWidget->raise();
    m_orderWidget->activateWindow();
}

void MainWindow::on_action_client_triggered()
{
    m_clientWidget->resize(900, 600);
    m_clientWidget->show();
    m_clientWidget->raise();
    m_clientWidget->activateWindow();
}

void MainWindow::on_action_monitor_triggered()
{
    m_monitorWidget->resize(1000, 700);
    m_monitorWidget->show();
    m_monitorWidget->raise();
    m_monitorWidget->activateWindow();
    m_monitorWidget->refreshClientList();
}

void MainWindow::on_action_ota_triggered()
{
    m_otaWidget->resize(1200, 1000);
    m_otaWidget->show();
    m_otaWidget->raise();
    m_otaWidget->activateWindow();
    m_otaWidget->refreshClientList();
    m_otaWidget->refreshVersionList();
}

void MainWindow::on_action_exit_triggered()
{
    stopServer();
    QTimer::singleShot(100, this, &MainWindow::close);
}
