#include "clientwidget.h"
#include "ui_clientwidget.h"
#include "businessmanager.h"
#include "stylehelper.h"
#include <QHeaderView>
#include <QDateTime>
#include <QMessageBox>

ClientWidget::ClientWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ClientWidget)
    , m_selectedClient(0)
{
    ui->setupUi(this);

    applyStyle();

    ui->tableWidget->setColumnCount(3);
    ui->tableWidget->setHorizontalHeaderLabels({"客户端句柄", "IP地址", "刷新时间"});
    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget, &QTableWidget::cellClicked, [=](int row, int column) {
        Q_UNUSED(column);
        QTableWidgetItem *item = ui->tableWidget->item(row, 0);
        if(!item) return;
        m_selectedClient = item->text().toLongLong();
    });

    // Client list change signals are wired in MainWindow (centralized connection management)

    slotClientListChanged();
}

void ClientWidget::applyStyle()
{
    this->setStyleSheet(StyleHelper::fullCommonStyle());
}

ClientWidget::~ClientWidget()
{
    delete ui;
}

void ClientWidget::slotClientListChanged()
{
    ui->tableWidget->setRowCount(0);
    auto clients = BusinessManager::getInstance()->getConnectedClients();

    int row = 0;
    for(auto &client : clients)
    {
        int fd = client.first;
        QString clientId = client.second;

        QString refreshTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

        ui->tableWidget->insertRow(row);
        ui->tableWidget->setItem(row, 0, new QTableWidgetItem(QString::number(fd)));
        ui->tableWidget->setItem(row, 1, new QTableWidgetItem(clientId));
        ui->tableWidget->setItem(row, 2, new QTableWidgetItem(refreshTime));
        row++;
    }
}

void ClientWidget::on_btn_kick_clicked()
{
    if(m_selectedClient == 0)
    {
        QMessageBox::warning(this, "警告", "请先选择客户端！");
        return;
    }

    BusinessManager::getInstance()->kickClient(m_selectedClient);
    m_selectedClient = 0;
    QMessageBox::information(this, "成功", "已强制下线客户端！");
}
