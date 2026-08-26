#include "otawidget.h"
#include "ui_otawidget.h"
#include "businessmanager.h"
#include "stylehelper.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QHeaderView>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>

OtaWidget::OtaWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::OtaWidget)
{
    ui->setupUi(this);
    applyStyle();

    ui->tableWidget_versions->setColumnCount(7);
    ui->tableWidget_versions->setHorizontalHeaderLabels(
        {"ID", "版本号", "文件名", "大小(KB)", "上传时间", "更新说明", "类型"});
    ui->tableWidget_versions->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_versions->setSelectionBehavior(QAbstractItemView::SelectRows);

    ui->comboBox_type->setCurrentIndex(0);

    ui->tableWidget_clients->setColumnCount(3);
    ui->tableWidget_clients->setHorizontalHeaderLabels({"FD", "客户端ID", "IP地址"});
    ui->tableWidget_clients->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_clients->setSelectionBehavior(QAbstractItemView::SelectRows);

    refreshVersionList();
    refreshClientList();
}

OtaWidget::~OtaWidget()
{
    delete ui;
}

void OtaWidget::applyStyle()
{
    QString style = StyleHelper::fullCommonStyle();
    style += "QCheckBox { color: #ecf0f1; }";
    this->setStyleSheet(style);
}

void OtaWidget::on_btn_select_file_clicked()
{
    m_selectedFilePath = QFileDialog::getOpenFileName(this, "选择客户端可执行程序", "", "可执行程序 (*)");
    if (!m_selectedFilePath.isEmpty()) {
        QFileInfo fi(m_selectedFilePath);
        ui->label_selected_file->setText(QString("已选择: %1 (%2 KB)")
            .arg(fi.fileName()).arg(fi.size() / 1024));
    }
}

void OtaWidget::on_btn_upload_clicked()
{
    QString version = ui->lineEdit_version->text().trimmed();
    QString desc = ui->lineEdit_desc->text().trimmed();
    bool force = ui->checkBox_force->isChecked();
    /* 0=APP, 1=SYSTEM; matches comboBox order */
    int type = ui->comboBox_type->currentIndex();

    if (version.isEmpty()) {
        QMessageBox::warning(this, "错误", "请输入版本号");
        return;
    }
    if (m_selectedFilePath.isEmpty()) {
        QMessageBox::warning(this, "错误", "请先选择要上传的文件");
        return;
    }

    QFile file(m_selectedFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "错误", "无法打开文件");
        return;
    }

    QByteArray fileData = file.readAll();
    file.close();

    QFileInfo fi(m_selectedFilePath);
    QString filename = fi.fileName();

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(fileData);
    QString sha256 = hash.result().toHex();

    // Save file first, then insert into DB (so we can clean up on failure)
    QString savePath = QString("%1/ota_packages/%2_%3")
        .arg(QCoreApplication::applicationDirPath())
        .arg(version).arg(filename);
    QDir().mkpath(QFileInfo(savePath).absolutePath());
    QFile outFile(savePath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "错误", "文件保存失败");
        return;
    }
    outFile.write(fileData);
    outFile.close();

    int ret = BusinessManager::getInstance()->otaAddVersion(
        version, filename, sha256, fileData.size(), desc, force ? 1 : 0, type);

    if (ret == 0) {
        QMessageBox::information(this, "成功", QString("版本 %1 上传成功!\n类型: %2\nSHA256: %3")
            .arg(version)
            .arg(type == 1 ? "SYSTEM" : "APP")
            .arg(sha256));
        ui->lineEdit_version->clear();
        ui->lineEdit_desc->clear();
        ui->checkBox_force->setChecked(false);
        ui->comboBox_type->setCurrentIndex(0);
        m_selectedFilePath.clear();
        ui->label_selected_file->setText("未选择文件");
        refreshVersionList();
    } else {
        QMessageBox::warning(this, "错误", "上传失败，版本号可能已存在");
        // DB insert failed: remove the file we saved earlier
        QFile::remove(savePath);
    }
}

void OtaWidget::refreshVersionList()
{
    ui->tableWidget_versions->setRowCount(0);

    ota_version_t list[50];
    int count = 0;
    BusinessManager::getInstance()->otaGetAllVersions(list, &count);

    if(count > 50) count = 50;

    for (int i = 0; i < count; i++) {
        ui->tableWidget_versions->insertRow(i);
        ui->tableWidget_versions->setItem(i, 0, new QTableWidgetItem(QString::number(list[i].id)));
        ui->tableWidget_versions->setItem(i, 1, new QTableWidgetItem(list[i].version));
        ui->tableWidget_versions->setItem(i, 2, new QTableWidgetItem(list[i].filename));
        ui->tableWidget_versions->setItem(i, 3, new QTableWidgetItem(QString::number(list[i].file_size / 1024)));
        ui->tableWidget_versions->setItem(i, 4, new QTableWidgetItem(list[i].upload_time));
        ui->tableWidget_versions->setItem(i, 5, new QTableWidgetItem(list[i].description));
        ui->tableWidget_versions->setItem(i, 6, new QTableWidgetItem(
            (list[i].type == OTA_TYPE_SYSTEM) ? "SYSTEM" : "APP"));
    }
}

void OtaWidget::slotOtaDataChanged()
{
    refreshVersionList();
}

void OtaWidget::refreshClientList()
{
    ui->tableWidget_clients->setRowCount(0);

    QList<QPair<int, QString>> clients = BusinessManager::getInstance()->getConnectedClients();
    for (int i = 0; i < clients.size(); i++) {
        ui->tableWidget_clients->insertRow(i);
        ui->tableWidget_clients->setItem(i, 0, new QTableWidgetItem(QString::number(clients[i].first)));
        ui->tableWidget_clients->setItem(i, 1, new QTableWidgetItem(clients[i].second));
        ui->tableWidget_clients->setItem(i, 2, new QTableWidgetItem("已连接"));
    }
}

void OtaWidget::on_btn_refresh_clicked()
{
    refreshVersionList();
    refreshClientList();
}

void OtaWidget::on_btn_push_selected_clicked()
{
    int row = ui->tableWidget_versions->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, "错误", "请先选择一个版本");
        return;
    }

    int clientRow = ui->tableWidget_clients->currentRow();
    if (clientRow < 0) {
        QMessageBox::warning(this, "错误", "请先选择一个在线客户端");
        return;
    }

    QTableWidgetItem *verItem = ui->tableWidget_versions->item(row, 1);
    QTableWidgetItem *clientItem = ui->tableWidget_clients->item(clientRow, 0);
    if(!verItem || !clientItem) return;

    QString version = verItem->text();
    int fd = clientItem->text().toInt();

    BusinessManager::getInstance()->otaPushToClient(fd, version);

    QMessageBox::information(this, "成功", QString("已推送版本 %1 到客户端 FD=%2").arg(version).arg(fd));
}

void OtaWidget::on_btn_push_all_clicked()
{
    int row = ui->tableWidget_versions->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, "错误", "请先选择一个版本");
        return;
    }

    QTableWidgetItem *verItem = ui->tableWidget_versions->item(row, 1);
    if(!verItem) return;
    QString version = verItem->text();

    BusinessManager::getInstance()->otaPushToAll(version);

    QMessageBox::information(this, "成功", QString("已推送版本 %1 到所有在线客户端").arg(version));
}

void OtaWidget::on_btn_delete_version_clicked()
{
    int row = ui->tableWidget_versions->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, "错误", "请先选择一个版本");
        return;
    }

    QTableWidgetItem *idItem = ui->tableWidget_versions->item(row, 0);
    QTableWidgetItem *verItem = ui->tableWidget_versions->item(row, 1);
    QTableWidgetItem *fnItem = ui->tableWidget_versions->item(row, 2);
    if(!idItem || !verItem || !fnItem) return;

    int id = idItem->text().toInt();
    QString version = verItem->text();
    QString filename = fnItem->text();

    if (QMessageBox::question(this, "确认", QString("确定要删除版本 %1 吗?").arg(version))
        == QMessageBox::Yes) {
        BusinessManager::getInstance()->otaDeleteVersion(id);

        QString filePath = QString("%1/ota_packages/%2_%3")
            .arg(QCoreApplication::applicationDirPath())
            .arg(version).arg(filename);
        QFile::remove(filePath);

        refreshVersionList();
    }
}
