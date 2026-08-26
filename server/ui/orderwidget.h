#ifndef ORDERWIDGET_H
#define ORDERWIDGET_H

#include <QWidget>
#include "common.h"

QT_BEGIN_NAMESPACE
namespace Ui { class OrderWidget; }
QT_END_NAMESPACE

class OrderWidget : public QWidget
{
    Q_OBJECT

public:
    OrderWidget(QWidget *parent = nullptr);
    ~OrderWidget();

public slots:
    void slotOrderDataChanged();

private slots:
    void on_btn_query_clicked();
    void on_btn_export_clicked();
    void slotTableItemClicked(int row, int column);

private:
    Ui::OrderWidget *ui;
    void refreshOrderTable(const QString& condition = QString());
    void applyStyle();
    order_info_t m_selectedOrder = {};
};

#endif // ORDERWIDGET_H