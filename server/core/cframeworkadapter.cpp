#include "cframeworkadapter.h"
#include <QDebug>

CNetEventThread::CNetEventThread(CFrameworkAdapter *adapter, QObject *parent)
    : QThread(parent)
    , m_adapter(adapter)
{
}

void CNetEventThread::stop()
{
    m_running = false;
    /* Do not call net_framework_stop here: CFrameworkAdapter::stop() owns the
     * lifecycle; calling stop here would double-stop inside net_framework_destroy
     * and cause a triple pthread_join. */
    wait();
}

void CNetEventThread::run()
{
    m_running = true;
    qDebug() << "C net event thread started (main Reactor)";

    if (m_adapter && m_adapter->getNetFramework()) {
        net_framework_start(m_adapter->getNetFramework());
    }

    qDebug() << "C net event thread exited";
}

CFrameworkAdapter::CFrameworkAdapter(QObject *parent)
    : QObject(parent)
    , m_netFramework(nullptr)
    , m_eventThread(nullptr)
    , m_backend(NET_BACKEND_EPOLL)
{
}

CFrameworkAdapter::~CFrameworkAdapter()
{
    stop();
}

bool CFrameworkAdapter::start(int port)
{
#ifdef USE_IO_URING
    if (net_probe_io_uring_support()) {
        m_backend = NET_BACKEND_IOURING;
    }
#endif

    m_netFramework = net_framework_create_with_backend(port, m_backend);
    if (m_netFramework == nullptr) {
        qDebug() << "failed to create network framework";
        return false;
    }

    m_netFramework->user_data = this;

    net_framework_set_callbacks(m_netFramework,
                                onAcceptCallback,
                                onRecvCallback,
                                onCloseCallback,
                                onErrorCallback);

    m_eventThread = new CNetEventThread(this, this);
    m_eventThread->start();

    const char *backend_name = (m_backend == NET_BACKEND_IOURING) ? "io_uring" : "epoll";
    emit signalLog(QString("network framework started, port: %1, backend: %2, thread pool: %3")
                   .arg(port).arg(backend_name).arg(NET_THREAD_POOL_SIZE));
    return true;
}

void CFrameworkAdapter::stop()
{
    if (m_eventThread) {
        m_eventThread->stop();
        delete m_eventThread;
        m_eventThread = nullptr;
    }

    if (m_netFramework) {
        net_framework_stop(m_netFramework);     /* stop reactor threads first */
        net_framework_destroy(m_netFramework);  /* then free resources */
        m_netFramework = nullptr;
    }

    emit signalLog("reactor network framework stopped");
}

bool CFrameworkAdapter::sendToClient(connection_t *conn, const QByteArray &rawData)
{
    if (conn == nullptr || m_netFramework == nullptr) {
        return false;
    }

    int ret = net_send_binary(conn, rawData.constData(), rawData.size());
    return ret == 0;
}

void CFrameworkAdapter::closeConnection(connection_t *conn)
{
    if (conn) net_close_connection(conn);
}

void CFrameworkAdapter::onAcceptCallback(connection_t *conn)
{
    if (!conn || !conn->nf) return;
    CFrameworkAdapter *adapter = (CFrameworkAdapter *)conn->nf->user_data;
    if (!adapter) return;
    /* No net_conn_ref needed: assign_connection already holds a reference, and
     * the Qt queued connection copies the argument value (the pointer itself). */
    emit adapter->signalClientConnected(conn, QString(conn->client_ip), conn->client_port);
    emit adapter->signalLog(QString("new client connected: %1:%2")
                            .arg(conn->client_ip).arg(conn->client_port));
}

void CFrameworkAdapter::onRecvCallback(connection_t *conn, const char *data, int len)
{
    if (!conn || !conn->nf || !data || len <= 0) return;
    CFrameworkAdapter *adapter = (CFrameworkAdapter *)conn->nf->user_data;
    if (!adapter) return;
    /* No net_conn_ref needed: recv_task_func already holds a reference, and the
     * slot runs synchronously in the same thread-pool task. */
    emit adapter->signalClientData(conn, QByteArray(data, len));
}

void CFrameworkAdapter::onCloseCallback(connection_t *conn)
{
    if (!conn || !conn->nf) return;
    CFrameworkAdapter *adapter = (CFrameworkAdapter *)conn->nf->user_data;
    if (!adapter) return;
    /* No net_conn_ref: epoll_close_conn / io_uring_close_conn still hold a
     * reference; the connection is about to be freed, so ref/unref would
     * cause use-after-free. */
    emit adapter->signalClientDisconnected(conn, QString(conn->client_ip), conn->client_port);
    emit adapter->signalLog(QString("client disconnected: %1:%2")
                            .arg(conn->client_ip).arg(conn->client_port));
}

void CFrameworkAdapter::onErrorCallback(connection_t *conn, int err)
{
    if (!conn || !conn->nf) return;
    CFrameworkAdapter *adapter = (CFrameworkAdapter *)conn->nf->user_data;
    if (!adapter) return;
    /* No net_conn_ref: on_error is called before close_conn, which frees the conn */
    emit adapter->signalClientError(conn, err);
    emit adapter->signalLog(QString("client error: %1:%2, errcode: %3")
                            .arg(conn->client_ip).arg(conn->client_port).arg(err));
}
