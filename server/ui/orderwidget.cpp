#include "orderwidget.h"
#include "ui_orderwidget.h"
#include "businessmanager.h"
#include "stylehelper.h"
#include <QMessageBox>
#include <QHeaderView>
#include <QFileDialog>
#include <QTextStream>
#include <QDateTime>
#include <QMutexLocker>
#include <QJsonParseError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDebug>

OrderWidget::OrderWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::OrderWidget)
{
    ui->setupUi(this);

    applyStyle();

    ui->tableWidget_order->setColumnCount(6);
    ui->tableWidget_order->setHorizontalHeaderLabels({"ID", "订单号", "会员UID", "总价", "支付状态", "创建时间"});
    ui->tableWidget_order->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_order->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget_order, &QTableWidget::cellClicked, this, &OrderWidget::slotTableItemClicked);

    ui->tableWidget_detail->setColumnCount(4);
    ui->tableWidget_detail->setHorizontalHeaderLabels({"商品ID", "商品名称", "数量", "小计"});
    ui->tableWidget_detail->horizontalHeader()->setStretchLastSection(true);

    refreshOrderTable();
}

void OrderWidget::applyStyle()
{
    this->setStyleSheet(StyleHelper::fullCommonStyle());
}

OrderWidget::~OrderWidget()
{
    delete ui;
}

void OrderWidget::refreshOrderTable(const QString& condition)
{
    ui->tableWidget_order->setRowCount(0);
    order_info_t list[100];
    int count = 0;

    if(condition.isEmpty())
    {
        BusinessManager::getInstance()->orderQueryAll(list, &count);
    }
    else
    {
        BusinessManager::getInstance()->orderQueryByCondition(condition, list, &count);
    }

    if(count > 100) count = 100;

    for(int i=0; i<count; i++)
    {
        ui->tableWidget_order->insertRow(i);
        ui->tableWidget_order->setItem(i, 0, new QTableWidgetItem(QString::number(list[i].id)));
        ui->tableWidget_order->setItem(i, 1, new QTableWidgetItem(list[i].order_id));
        ui->tableWidget_order->setItem(i, 2, new QTableWidgetItem(list[i].member_uid));
        ui->tableWidget_order->setItem(i, 3, new QTableWidgetItem(QString::number(list[i].total, 'f', 2)));
        ui->tableWidget_order->setItem(i, 4, new QTableWidgetItem(list[i].pay_status == 1 ? "已支付" : "未支付"));
        QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
        ui->tableWidget_order->setItem(i, 5, new QTableWidgetItem(time.toString("yyyy-MM-dd hh:mm:ss")));
    }
}

void OrderWidget::slotTableItemClicked(int row, int column)
{
    Q_UNUSED(column);
    QTableWidgetItem *item = ui->tableWidget_order->item(row, 0);
    if(!item) return;
    int orderId = item->text().toInt();

    order_info_t order;
    BusinessManager::getInstance()->orderQueryById(orderId, &order);

    ui->tableWidget_detail->setRowCount(0);
    if(strlen(order.goods_list) > 0)
    {
        QJsonParseError jsonError;
        QJsonDocument doc = QJsonDocument::fromJson(order.goods_list, &jsonError);
        if(jsonError.error == QJsonParseError::NoError && doc.isArray())
        {
            QJsonArray array = doc.array();
            for(int i=0; i<array.size(); i++)
            {
                QJsonObject obj = array[i].toObject();
                ui->tableWidget_detail->insertRow(i);
                ui->tableWidget_detail->setItem(i, 0, new QTableWidgetItem(QString::number(obj["goods_id"].toInt())));
                ui->tableWidget_detail->setItem(i, 1, new QTableWidgetItem(obj["goods_name"].toString()));
                ui->tableWidget_detail->setItem(i, 2, new QTableWidgetItem(QString::number(obj["num"].toInt())));
                ui->tableWidget_detail->setItem(i, 3, new QTableWidgetItem(QString::number(obj["subtotal"].toDouble(), 'f', 2)));
            }
        }
    }
}

void OrderWidget::on_btn_query_clicked()
{
    QString condition = ui->lineEdit_condition->text().trimmed();
    qDebug() << "Query orders, condition:" << condition;
    refreshOrderTable(condition);
}

void OrderWidget::on_btn_export_clicked()
{
    QFile file("./order_report.csv");
    if(file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream out(&file);
        out << "订单ID,会员UID,总价,支付状态,创建时间\n";

        order_info_t list[100];
        int count = 0;
        
        BusinessManager::getInstance()->orderQueryAll(list, &count);

        if(count > 100) count = 100;

        for(int i=0; i<count; i++)
        {
            QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
            out << list[i].order_id << ","
                << list[i].member_uid << ","
                << list[i].total << ","
                << (list[i].pay_status == 1 ? "已支付" : "未支付") << ","
                << time.toString("yyyy-MM-dd hh:mm:ss") << "\n";
        }
        file.close();
        QMessageBox::information(this, "成功", "订单报表导出成功！");
    }
}

void OrderWidget::slotOrderDataChanged()
{
    refreshOrderTable(ui->lineEdit_condition->text().trimmed());
}