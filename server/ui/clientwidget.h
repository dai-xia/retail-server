#ifndef CLIENTWIDGET_H
#define CLIENTWIDGET_H

#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class ClientWidget; }
QT_END_NAMESPACE

class ClientWidget : public QWidget
{
    Q_OBJECT

public:
    ClientWidget(QWidget *parent = nullptr);
    ~ClientWidget();

public slots:
    void slotClientListChanged();

private slots:
    void on_btn_kick_clicked();

private:
    Ui::ClientWidget *ui;
    void applyStyle();
    int m_selectedClient;
};

#endif // CLIENTWIDGET_H
