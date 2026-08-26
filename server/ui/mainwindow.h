#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "memberwidget.h"
#include "goodswidget.h"
#include "orderwidget.h"
#include "clientwidget.h"
#include "otawidget.h"
#include "monitor_widget.h"
#include "businessmanager.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void slotAddLog(QString log);
    void on_action_member_triggered();
    void on_action_goods_triggered();
    void on_action_order_triggered();
    void on_action_client_triggered();
    void on_action_monitor_triggered();
    void on_action_ota_triggered();
    void on_action_exit_triggered();

private:
    Ui::MainWindow *ui;

    MemberWidget* m_memberWidget;
    GoodsWidget* m_goodsWidget;
    OrderWidget* m_orderWidget;
    ClientWidget* m_clientWidget;
    MonitorWidget* m_monitorWidget;
    OtaWidget* m_otaWidget;

    void initSystem();
    void startServer();
    void stopServer();
    void applyModernStyle();
};
#endif // MAINWINDOW_H
