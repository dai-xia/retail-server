#ifndef GOODSWIDGET_H
#define GOODSWIDGET_H

#include <QWidget>
#include "common.h"

QT_BEGIN_NAMESPACE
namespace Ui { class GoodsWidget; }
QT_END_NAMESPACE

class GoodsWidget : public QWidget
{
    Q_OBJECT

public:
    GoodsWidget(QWidget *parent = nullptr);
    ~GoodsWidget();

public slots:
    void slotGoodsDataChanged();
    void updateClientComboBox();

private slots:
    void on_btn_add_clicked();
    void on_btn_update_clicked();
    void on_btn_delete_clicked();
    void on_btn_refresh_clicked();
    void slotTableItemClicked(int row, int column);
    void slotClientSelectionChanged(int index);

private:
    Ui::GoodsWidget *ui;
    void refreshGoodsTable();
    void applyStyle();
    QString m_currentClientId;
    goods_info_t m_selectedGoods = {};
};

#endif // GOODSWIDGET_H