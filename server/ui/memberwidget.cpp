#include "memberwidget.h"
#include "ui_memberwidget.h"
#include "businessmanager.h"
#include "stylehelper.h"
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QMutexLocker>
#include <QVariant>
#include <QMap>

MemberWidget::MemberWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::MemberWidget)
{
    ui->setupUi(this);

    applyStyle();

    ui->tableWidget->setColumnCount(6);
    ui->tableWidget->setHorizontalHeaderLabels({"UID", "姓名", "手机号", "余额", "类型", "注册时间"});
    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget, &QTableWidget::cellClicked, this, &MemberWidget::slotTableItemClicked);

    ui->tableWidget_balanceLog->setColumnCount(6);
    ui->tableWidget_balanceLog->setHorizontalHeaderLabels({"时间", "类型", "金额", "变动前", "变动后", "备注"});
    ui->tableWidget_balanceLog->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_balanceLog->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Data change signals are wired in MainWindow (centralized connection management)

    refreshMemberTable();
}

void MemberWidget::applyStyle()
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
    this->setStyleSheet(style);
}

MemberWidget::~MemberWidget()
{
    delete ui;
}

void MemberWidget::refreshMemberTable()
{
    ui->tableWidget->setRowCount(0);
    member_info_t list[100];
    int count = 0;
    
    BusinessManager::getInstance()->memberQueryAll(list, &count);

    if(count > 100) count = 100;

    for(int i=0; i<count; i++)
    {
        ui->tableWidget->insertRow(i);
        ui->tableWidget->setItem(i, 0, new QTableWidgetItem(list[i].uid));
        ui->tableWidget->setItem(i, 1, new QTableWidgetItem(list[i].name));
        ui->tableWidget->setItem(i, 2, new QTableWidgetItem(list[i].phone));
        ui->tableWidget->setItem(i, 3, new QTableWidgetItem(QString::number(list[i].balance, 'f', 2)));
        QString typeStr = (list[i].member_type == 1) ? "管理员" : "普通客户";
        ui->tableWidget->setItem(i, 4, new QTableWidgetItem(typeStr));
        QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
        ui->tableWidget->setItem(i, 5, new QTableWidgetItem(time.toString("yyyy-MM-dd hh:mm:ss")));
    }
}

void MemberWidget::slotTableItemClicked(int row, int column)
{
    Q_UNUSED(column);
    QTableWidgetItem *item = ui->tableWidget->item(row, 0);
    if(!item) return;
    QString uid = item->text();
    
    BusinessManager::getInstance()->memberQueryByUid(uid, &m_selectedMember);

    ui->lineEdit_uid->setText(m_selectedMember.uid);
    ui->lineEdit_name->setText(m_selectedMember.name);
    ui->lineEdit_phone->setText(m_selectedMember.phone);
    ui->doubleSpinBox_balance->setValue(m_selectedMember.balance);
    ui->lineEdit_password->clear();
    ui->comboBox_type->setCurrentIndex(m_selectedMember.member_type);

    refreshBalanceLog(uid);
}

void MemberWidget::on_btn_add_clicked()
{
    QString uid = ui->lineEdit_uid->text().trimmed();
    QString name = ui->lineEdit_name->text().trimmed();
    QString phone = ui->lineEdit_phone->text().trimmed();
    double balance = ui->doubleSpinBox_balance->value();
    QString password = ui->lineEdit_password->text().trimmed();

    if(uid.isEmpty() || name.isEmpty())
    {
        QMessageBox::warning(this, "警告", "UID和姓名不能为空！");
        return;
    }

    if(password.isEmpty())
    {
        QMessageBox::warning(this, "警告", "密码不能为空！");
        return;
    }

    int memberType = ui->comboBox_type->currentIndex(); // 0=normal customer, 1=admin
    int ret = BusinessManager::getInstance()->memberRegister(uid, name, phone, balance, password,
                                                              QString(), QString(), memberType);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "会员添加成功！");
        refreshMemberTable();
    }
    else
    {
        QMessageBox::warning(this, "警告", "会员添加失败！");
    }
}

void MemberWidget::on_btn_update_clicked()
{
    if(m_selectedMember.uid[0] == 0)
    {
        QMessageBox::warning(this, "警告", "请先选择会员！");
        return;
    }

    QString uid = ui->lineEdit_uid->text().trimmed();
    double balance = ui->doubleSpinBox_balance->value();

    int ret = BusinessManager::getInstance()->memberUpdateBalance(uid, balance);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "会员余额更新成功！");
        refreshMemberTable();
    }
    else
    {
        QMessageBox::warning(this, "警告", "会员更新失败！");
    }
}

void MemberWidget::on_btn_delete_clicked()
{
    if(m_selectedMember.uid[0] == 0)
    {
        QMessageBox::warning(this, "警告", "请先选择会员！");
        return;
    }

    if(QMessageBox::question(this, "确认", "确定要删除这个会员吗？") != QMessageBox::Yes)
    {
        return;
    }

    int ret = BusinessManager::getInstance()->memberDelete(m_selectedMember.uid);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "会员删除成功！");
        refreshMemberTable();
        
        m_selectedMember.uid[0] = 0;
        ui->lineEdit_uid->clear();
        ui->lineEdit_name->clear();
        ui->lineEdit_phone->clear();
        ui->doubleSpinBox_balance->setValue(0);
    }
    else
    {
        QMessageBox::warning(this, "警告", "会员删除失败！");
    }
}

void MemberWidget::on_btn_refresh_clicked()
{
    refreshMemberTable();
}

void MemberWidget::slotMemberDataChanged()
{
    refreshMemberTable();
}

void MemberWidget::refreshBalanceLog(const QString& uid)
{
    ui->tableWidget_balanceLog->setRowCount(0);

    QList<QMap<QString, QVariant>> logList;
    int ret = BusinessManager::getInstance()->balanceLogQuery(uid, logList, 50);
    if(ret != 0) return;

    for(int i = 0; i < logList.size(); i++)
    {
        const auto& entry = logList[i];
        int type = entry["type"].toInt();
        QString typeStr;
        if(type == 1) typeStr = "充值";
        else if(type == 2) typeStr = "消费";
        else if(type == 3) typeStr = "管理员调整";
        else typeStr = "未知";

        double amount = entry["amount"].toDouble();
        QString amountStr = (type == 2) ? QString("-%1").arg(amount, 0, 'f', 2)
                                        : QString("+%1").arg(amount, 0, 'f', 2);

        ui->tableWidget_balanceLog->insertRow(i);
        ui->tableWidget_balanceLog->setItem(i, 0, new QTableWidgetItem(entry["create_time"].toString()));
        ui->tableWidget_balanceLog->setItem(i, 1, new QTableWidgetItem(typeStr));
        ui->tableWidget_balanceLog->setItem(i, 2, new QTableWidgetItem(amountStr));
        ui->tableWidget_balanceLog->setItem(i, 3, new QTableWidgetItem(QString::number(entry["old_balance"].toDouble(), 'f', 2)));
        ui->tableWidget_balanceLog->setItem(i, 4, new QTableWidgetItem(QString::number(entry["new_balance"].toDouble(), 'f', 2)));
        ui->tableWidget_balanceLog->setItem(i, 5, new QTableWidgetItem(entry["remark"].toString()));
    }
}