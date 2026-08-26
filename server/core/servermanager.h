#ifndef SERVERMANAGER_H
#define SERVERMANAGER_H

#include <QObject>
#include <QMap>
#include "cframeworkadapter.h"
#include "net_framework.h"

class ServerManager : public QObject
{
    Q_OBJECT
public:
    explicit ServerManager(QObject *parent = nullptr);
    static ServerManager* getInstance();

    // Server start / stop
    bool startServer(const QString &ip = "127.0.0.1", int port = 9090);
    void stopServer();
    bool isServerRunning();

    // fd-based client management (called by BusinessManager)
    void sendToClient(int fd, const QByteArray &rawData);
    void sendToAllClients(const QByteArray &rawData);
    void closeClient(int fd);
    QList<QPair<int, QString>> getConnectedClients();  // returns (fd, ip) pairs
    QString getClientIp(int fd);

signals:
    // fd-based signals (connected by BusinessManager)
    void signalClientConnected(int fd, QString ip);
    void signalClientData(int fd, QByteArray rawData);
    void signalClientDisconnected(int fd);

private slots:
    // From CFrameworkAdapter (connection_t* based)
    void slotClientConnected(connection_t *conn, QString ip, int port);
    void slotClientDisconnected(connection_t *conn, QString ip, int port);
    void slotRecvClientData(connection_t *conn, QByteArray rawData);
    void slotClientError(connection_t *conn, int err);
    void slotFrameworkLog(QString log);

private:
    CFrameworkAdapter* m_cFramework;
    QMap<int, connection_t*> m_clientMap; // fd -> connection (internal)
    QMutex m_mapMutex;
    QString m_serverIP;
    int m_serverPort;
    bool m_isRunning;
};

#endif // SERVERMANAGER_H
