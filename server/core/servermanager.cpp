#include "servermanager.h"
#include <QDebug>
#include <QMetaType>
#include <QMutexLocker>

static QMutex s_serverMutex;
static ServerManager* s_serverInstance = nullptr;

ServerManager::ServerManager(QObject *parent)
    : QObject(parent)
    , m_cFramework(nullptr)
    , m_serverIP("127.0.0.1")
    , m_serverPort(9090)
    , m_isRunning(false)
{
    m_cFramework = new CFrameworkAdapter(this);

    /* DirectConnection: callbacks fire on thread-pool worker threads and must
     * be handled synchronously here, otherwise the connection_t* could dangle. */
    connect(m_cFramework, &CFrameworkAdapter::signalClientConnected,
            this, &ServerManager::slotClientConnected, Qt::DirectConnection);
    connect(m_cFramework, &CFrameworkAdapter::signalClientDisconnected,
            this, &ServerManager::slotClientDisconnected, Qt::DirectConnection);
    connect(m_cFramework, &CFrameworkAdapter::signalClientData,
            this, &ServerManager::slotRecvClientData, Qt::DirectConnection);
    connect(m_cFramework, &CFrameworkAdapter::signalClientError,
            this, &ServerManager::slotClientError, Qt::DirectConnection);
    connect(m_cFramework, &CFrameworkAdapter::signalLog,
            this, &ServerManager::slotFrameworkLog, Qt::DirectConnection);

    qDebug() << "ServerManager created, using C-style network framework";
}

ServerManager* ServerManager::getInstance()
{
    if (s_serverInstance == nullptr) {
        QMutexLocker locker(&s_serverMutex);
        if (s_serverInstance == nullptr) {
            s_serverInstance = new ServerManager();
        }
    }
    return s_serverInstance;
}

bool ServerManager::startServer(const QString& ip, int port)
{
    if(m_isRunning) return true;

    m_serverIP = ip;
    m_serverPort = port;

    qDebug() << "starting server, listen IP:" << ip << ", port:" << port;

    if(!m_cFramework->start(port))
    {
        qDebug() << "server start failed: C framework failed to start";
        return false;
    }

    m_isRunning = true;
    qDebug() << "server started, listening on" << ip << ":" << port;
    return true;
}

void ServerManager::stopServer()
{
    if(!m_isRunning) return;

    m_cFramework->stop();

    {
        QMutexLocker locker(&m_mapMutex);
        m_clientMap.clear();
    }

    m_isRunning = false;
}

bool ServerManager::isServerRunning()
{
    return m_isRunning;
}

void ServerManager::sendToClient(int fd, const QByteArray &rawData)
{
    QMutexLocker locker(&m_mapMutex);
    connection_t *conn = m_clientMap.value(fd, nullptr);
    if (conn != nullptr) {
        m_cFramework->sendToClient(conn, rawData);
    } else {
        qDebug() << "send failed: no connection for fd=" << fd;
    }
}

void ServerManager::sendToAllClients(const QByteArray &rawData)
{
    QMutexLocker locker(&m_mapMutex);
    for (auto it = m_clientMap.begin(); it != m_clientMap.end(); ++it) {
        m_cFramework->sendToClient(it.value(), rawData);
    }
}

void ServerManager::closeClient(int fd)
{
    QMutexLocker locker(&m_mapMutex);
    connection_t *conn = m_clientMap.value(fd, nullptr);
    if (conn != nullptr) {
        m_cFramework->closeConnection(conn);
    }
}

QList<QPair<int, QString>> ServerManager::getConnectedClients()
{
    QMutexLocker locker(&m_mapMutex);
    QList<QPair<int, QString>> result;
    for (auto it = m_clientMap.begin(); it != m_clientMap.end(); ++it) {
        connection_t *conn = it.value();
        result.append(qMakePair(it.key(), QString(conn->client_ip)));
    }
    return result;
}

QString ServerManager::getClientIp(int fd)
{
    QMutexLocker locker(&m_mapMutex);
    connection_t *conn = m_clientMap.value(fd, nullptr);
    if (conn) {
        return QString(conn->client_ip);
    }
    return QString();
}

// ========== Slots ==========

void ServerManager::slotClientConnected(connection_t *conn, QString ip, int port)
{
    if(conn == nullptr) return;

    if (conn->closed) {
        return;
    }

    {
        QMutexLocker locker(&m_mapMutex);
        m_clientMap.insert(conn->fd, conn);
    }

    emit signalClientConnected(conn->fd, ip);

    qDebug() << "new client connected:" << ip << ":" << port << "fd=" << conn->fd;
}

void ServerManager::slotClientDisconnected(connection_t *conn, QString ip, int port)
{
    if(conn == nullptr) return;

    int fd = conn->fd;

    {
        QMutexLocker locker(&m_mapMutex);
        if (m_clientMap.value(fd) == conn) {
            m_clientMap.remove(fd);
        }
    }

    emit signalClientDisconnected(fd);

    qDebug() << "client disconnected:" << ip << ":" << port << "fd=" << fd;
}

void ServerManager::slotRecvClientData(connection_t *conn, QByteArray rawData)
{
    if(conn == nullptr) return;

    /* Forward to BusinessManager via the fd-based signal */
    emit signalClientData(conn->fd, rawData);
}

void ServerManager::slotClientError(connection_t *conn, int err)
{
    if(conn == nullptr) return;

    int fd = conn->fd;

    {
        QMutexLocker locker(&m_mapMutex);
        if (m_clientMap.value(fd) == conn) {
            m_clientMap.remove(fd);
        }
    }

    emit signalClientDisconnected(fd);
}

void ServerManager::slotFrameworkLog(QString log)
{
    Q_UNUSED(log);
}
