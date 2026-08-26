#ifndef CFRAMEWORKADAPTER_H
#define CFRAMEWORKADAPTER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <atomic>
#include "net_framework.h"

class CFrameworkAdapter;

class CNetEventThread : public QThread
{
    Q_OBJECT
public:
    explicit CNetEventThread(CFrameworkAdapter *adapter, QObject *parent = nullptr);
    void stop();

protected:
    void run() override;

private:
    CFrameworkAdapter *m_adapter;
    std::atomic<bool> m_running{false};
};

class CFrameworkAdapter : public QObject
{
    Q_OBJECT
public:
    explicit CFrameworkAdapter(QObject *parent = nullptr);
    ~CFrameworkAdapter();

    bool start(int port);
    void stop();

    bool sendToClient(connection_t *conn, const QByteArray &rawData);
    void closeConnection(connection_t *conn);

    net_framework_t* getNetFramework() { return m_netFramework; }

    static void onAcceptCallback(connection_t *conn);
    static void onRecvCallback(connection_t *conn, const char *data, int len);
    static void onCloseCallback(connection_t *conn);
    static void onErrorCallback(connection_t *conn, int err);

signals:
    void signalClientConnected(connection_t *conn, QString ip, int port);
    void signalClientData(connection_t *conn, QByteArray rawData);
    void signalClientDisconnected(connection_t *conn, QString ip, int port);
    void signalClientError(connection_t *conn, int err);
    void signalLog(QString log);

private:
    net_framework_t *m_netFramework;
    CNetEventThread *m_eventThread;
    net_backend_t m_backend;
};

#endif
