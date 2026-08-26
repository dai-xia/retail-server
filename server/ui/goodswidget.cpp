#include "goodswidget.h"
#include "ui_goodswidget.h"
#include "businessmanager.h"
#include "stylehelper.h"
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QMutexLocker>

GoodsWidget::GoodsWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::GoodsWidget)
    , m_currentClientId("")
{
    ui->setupUi(this);

    applyStyle();

    ui->tableWidget->setColumnCount(6);
    ui->tableWidget->setHorizontalHeaderLabels({"ID", "名称", "单价", "单位", "库存", "创建时间"});
    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget, &QTableWidget::cellClicked, this, &GoodsWidget::slotTableItemClicked);

    // Data change signals are wired in MainWindow (centralized connection management)

    connect(ui->comboBox_client, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &GoodsWidget::slotClientSelectionChanged);

    // signalClientListChanged -> updateClientComboBox is connected in MainWindow

    updateClientComboBox();

    refreshGoodsTable();
}

void GoodsWidget::applyStyle()
{
    QString style = StyleHelper::fullCommonStyle();
    style += "QDoubleSpinBox {"
             "    background-color: #0f1419;"
             "    border: 2px solid #2d3a4a;"
             "    border-radius: 6px;"
             "    padding: 8px;"
             "    color: #ecf0f1;"
             "    font-size: 15px;"
             "}";
    style += "QComboBox {"
             "    background-color: #0f1419;"
             "    border: 2px solid #2d3a4a;"
             "    border-radius: 6px;"
             "    padding: 8px;"
             "    color: #ecf0f1;"
             "    min-width: 120px;"
             "    font-size: 15px;"
             "}";
    style += "QComboBox::drop-down {"
             "    border: none;"
             "    width: 30px;"
             "}";
    style += "QComboBox QAbstractItemView {"
             "    background-color: #16213e;"
             "    color: #ecf0f1;"
             "    selection-background-color: #e94560;"
             "}";
    this->setStyleSheet(style);
}

GoodsWidget::~GoodsWidget()
{
    delete ui;
}

void GoodsWidget::refreshGoodsTable()
{
    ui->tableWidget->setRowCount(0);
    goods_info_t list[100];
    int count = 0;

    if(m_currentClientId.isEmpty())
    {
        BusinessManager::getInstance()->goodsQueryAll(list, &count);
    }
    else
    {
        BusinessManager::getInstance()->goodsQueryByClientId(m_currentClientId, list, &count);
    }

    if(count > 100) count = 100;

    for(int i=0; i<count; i++)
    {
        ui->tableWidget->insertRow(i);
        ui->tableWidget->setItem(i, 0, new QTableWidgetItem(QString::number(list[i].id)));
        ui->tableWidget->setItem(i, 1, new QTableWidgetItem(list[i].name));
        ui->tableWidget->setItem(i, 2, new QTableWidgetItem(QString::number(list[i].price, 'f', 2)));
        ui->tableWidget->setItem(i, 3, new QTableWidgetItem(list[i].unit));
        ui->tableWidget->setItem(i, 4, new QTableWidgetItem(QString::number(list[i].stock)));
        QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
        ui->tableWidget->setItem(i, 5, new QTableWidgetItem(time.toString("yyyy-MM-dd hh:mm:ss")));
    }
}

void GoodsWidget::updateClientComboBox()
{
    QString oldClientId = m_currentClientId;
    ui->comboBox_client->clear();

    auto clients = BusinessManager::getInstance()->getConnectedClients();
    for(auto &c : clients)
    {
        QString clientId = c.second;

        QString displayText = clientId;
        QString dataValue = clientId;

        if(ui->comboBox_client->findData(dataValue) < 0)
        {
            ui->comboBox_client->addItem(displayText, dataValue);
        }
    }

    if(ui->comboBox_client->count() > 0)
    {
        // Keep previous selection if still present, otherwise fall back to first
        int index = ui->comboBox_client->findData(oldClientId);
        if(index < 0) index = 0;
        ui->comboBox_client->setCurrentIndex(index);
        m_currentClientId = ui->comboBox_client->itemData(index).toString();
    }
    else
    {
        m_currentClientId = "";
    }

    refreshGoodsTable();
}

void GoodsWidget::slotClientSelectionChanged(int index)
{
    if(index >= 0)
    {
        m_currentClientId = ui->comboBox_client->itemData(index).toString();
        refreshGoodsTable();
    }
}

void GoodsWidget::slotTableItemClicked(int row, int column)
{
    Q_UNUSED(column);
    QTableWidgetItem *item = ui->tableWidget->item(row, 0);
    if(!item) return;
    int id = item->text().toInt();
    
    BusinessManager::getInstance()->goodsQueryById(id, &m_selectedGoods);

    ui->lineEdit_name->setText(m_selectedGoods.name);
    ui->doubleSpinBox_price->setValue(m_selectedGoods.price);
    ui->lineEdit_unit->setText(m_selectedGoods.unit);
    ui->spinBox_stock->setValue(m_selectedGoods.stock);
}

void GoodsWidget::on_btn_add_clicked()
{
    QString name = ui->lineEdit_name->text().trimmed();
    double price = ui->doubleSpinBox_price->value();
    QString unit = ui->lineEdit_unit->text().trimmed();
    int stock = ui->spinBox_stock->value();

    if(name.isEmpty() || unit.isEmpty())
    {
        QMessageBox::warning(this, "警告", "商品名称和单位不能为空！");
        return;
    }

    int ret = BusinessManager::getInstance()->goodsAdd(m_currentClientId, name, price, unit, stock);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "商品添加成功！");
        refreshGoodsTable();
    }
    else
    {
        QMessageBox::warning(this, "警告", "商品添加失败！");
    }
}

void GoodsWidget::on_btn_update_clicked()
{
    if(m_selectedGoods.id == 0)
    {
        QMessageBox::warning(this, "警告", "请先选择商品！");
        return;
    }

    QString name = ui->lineEdit_name->text().trimmed();
    double price = ui->doubleSpinBox_price->value();
    QString unit = ui->lineEdit_unit->text().trimmed();
    int stock = ui->spinBox_stock->value();

    int ret = BusinessManager::getInstance()->goodsUpdate(m_currentClientId, m_selectedGoods.name, name, price, unit, stock);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "商品更新成功！");
        refreshGoodsTable();
    }
    else
    {
        QMessageBox::warning(this, "警告", "商品更新失败！");
    }
}

void GoodsWidget::on_btn_delete_clicked()
{
    if(m_selectedGoods.id == 0)
    {
        QMessageBox::warning(this, "警告", "请先选择商品！");
        return;
    }

    if(QMessageBox::question(this, "确认", "确定要删除这个商品吗？") != QMessageBox::Yes)
    {
        return;
    }

    int ret = BusinessManager::getInstance()->goodsDelete(m_currentClientId, m_selectedGoods.name);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "商品删除成功！");
        refreshGoodsTable();
        m_selectedGoods.id = 0;
        ui->lineEdit_name->clear();
        ui->doubleSpinBox_price->setValue(0);
        ui->lineEdit_unit->clear();
        ui->spinBox_stock->setValue(0);
    }
    else
    {
        QMessageBox::warning(this, "警告", "商品删除失败！");
    }
}

void GoodsWidget::on_btn_refresh_clicked()
{
    refreshGoodsTable();
}

void GoodsWidget::slotGoodsDataChanged()
{
    refreshGoodsTable();
}