#ifndef MEMBERWIDGET_H
#define MEMBERWIDGET_H

#include <QWidget>
#include "common.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MemberWidget; }
QT_END_NAMESPACE

class MemberWidget : public QWidget
{
    Q_OBJECT

public:
    MemberWidget(QWidget *parent = nullptr);
    ~MemberWidget();

public slots:
    void slotMemberDataChanged();

private slots:
    void on_btn_add_clicked();
    void on_btn_update_clicked();
    void on_btn_delete_clicked();
    void on_btn_refresh_clicked();
    void slotTableItemClicked(int row, int column);

private:
    Ui::MemberWidget *ui;
    void refreshMemberTable();
    void refreshBalanceLog(const QString& uid);
    void applyStyle();
    member_info_t m_selectedMember = {};
};

#endif // MEMBERWIDGET_H