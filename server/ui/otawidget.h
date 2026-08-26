#ifndef OTAWIDGET_H
#define OTAWIDGET_H

#include <QWidget>
#include <QString>
#include "common.h"

QT_BEGIN_NAMESPACE
namespace Ui { class OtaWidget; }
QT_END_NAMESPACE

class OtaWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OtaWidget(QWidget *parent = nullptr);
    ~OtaWidget();

    void refreshClientList();
    void refreshVersionList();

public slots:
    void slotOtaDataChanged();

private slots:
    void on_btn_select_file_clicked();
    void on_btn_upload_clicked();
    void on_btn_refresh_clicked();
    void on_btn_push_selected_clicked();
    void on_btn_push_all_clicked();
    void on_btn_delete_version_clicked();

private:
    Ui::OtaWidget *ui;
    QString m_selectedFilePath;
    void applyStyle();
};

#endif
