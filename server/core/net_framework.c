/* net_framework.c — shared base code + factory methods */
#include "net_framework.h"

/* A parsed frame is wrapped in a recv_task and dispatched to the business
 * thread pool, so the IO thread never blocks on business logic. */
typedef struct {
    connection_t *conn;
    char *data;
    int len;
    net_framework_t *nf;
} recv_task_t;

static void recv_task_func(void *arg)
{
    recv_task_t *task = (recv_task_t *)arg;
    if (task->nf && task->nf->on_recv && !task->conn->closed) {
        task->nf->on_recv(task->conn, task->data, task->len);
    }
    net_conn_unref(task->conn);
    free(task->data);
    free(task);
}

/* Cleanup invoked when the pool is destroyed / task is dropped, to avoid leaks */
static void recv_task_cleanup(void *arg)
{
    recv_task_t *task = (recv_task_t *)arg;
    net_conn_unref(task->conn);
    free(task->data);
    free(task);
}

int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief Core TCP frame parser: splits the byte stream into complete frames
 *        (handles sticky/half packets).
 *
 * Wire protocol: [4-byte network-order length header][business payload].
 * Each complete frame is dispatched to the thread pool via on_recv.
 *
 * @return 0 on success, -1 on malformed data / buffer overflow (close conn)
 */
int net_parse_frames(connection_t *c, net_framework_t *nf, int bytes_read)
{
    if (bytes_read <= 0 || c->rlen + bytes_read > NET_READ_BUF_SIZE) {
        return -1;
    }
    c->rlen += bytes_read;

    while (1) {
        if (c->rlen < NET_HEAD_LEN) break;

        if (c->expect_len == 0) {
            uint32_t head_val;
            memcpy(&head_val, c->rbuf_ptr, NET_HEAD_LEN);
            c->expect_len = ntohl(head_val);

            /* Reject oversized frames to guard against malicious packets */
            if (c->expect_len > (uint32_t)(NET_READ_BUF_SIZE - NET_HEAD_LEN)) {
                return -1;
            }
        }

        if (c->rlen < (int)(NET_HEAD_LEN + c->expect_len)) break;

        int msg_len = (int)c->expect_len;
        char *msg_data = malloc(msg_len + 1);
        memcpy(msg_data, c->rbuf_ptr + NET_HEAD_LEN, msg_len);
        msg_data[msg_len] = '\0';

        recv_task_t *task = malloc(sizeof(recv_task_t));
        task->conn = c;
        task->data = msg_data;
        task->len = msg_len;
        task->nf = nf;

        net_conn_ref(c); /* keep the connection alive while the task is pending */
        thread_pool_add_task(nf->thread_pool, recv_task_func, task, recv_task_cleanup);

        int total = NET_HEAD_LEN + msg_len;
        c->rlen -= total;
        if (c->rlen > 0) memmove(c->rbuf_ptr, c->rbuf_ptr + total, c->rlen);
        c->expect_len = 0;
    }
    return 0;
}

void net_queue_write(connection_t *c, const char *data, int len)
{
    write_task_t *wt = calloc(1, sizeof(write_task_t));
    wt->data = malloc(len);
    memcpy(wt->data, data, len);
    wt->len = len;

    int need_submit = 0;

    pthread_mutex_lock(&c->write_lock);
    if (c->write_tail) {
        c->write_tail->next = wt;
    } else {
        c->write_head = wt;
    }
    c->write_tail = wt;
    c->write_count++;

    if (c->write_head == wt && !c->closed) {
        need_submit = 1;
    }
    pthread_mutex_unlock(&c->write_lock);

    if (need_submit && c->nf && c->nf->ops && c->nf->ops->submit_write) {
        c->nf->ops->submit_write(c);
    }
}

void net_conn_ref(connection_t *c)
{
    if (c) __sync_fetch_and_add(&c->ref, 1);
}

void net_conn_unref(connection_t *c)
{
    if (c) {
        int old = __sync_fetch_and_sub(&c->ref, 1);
        if (old == 1) {
            write_task_t *wt = c->write_head;
            while (wt) {
                write_task_t *next = wt->next;
                free(wt->data);
                free(wt);
                wt = next;
            }
            if (c->backend_data) {
                free(c->backend_data);
                c->backend_data = NULL;
            }
            pthread_mutex_destroy(&c->write_lock);
            free(c);
        }
    }
}

/**
 * @brief Send a JSON packet (prepends the 4-byte length header)
 * @return 0 on success, -1 on failure
 */
int net_send_packet(connection_t *conn, const char *json_data)
{
    if (!conn || !json_data || conn->closed) return -1;
    int json_len = strlen(json_data);
    if (json_len > NET_BUF_SIZE - NET_HEAD_LEN) return -1;

    int total = NET_HEAD_LEN + json_len;
    char *packet = malloc(total);
    uint32_t head = htonl((uint32_t)json_len);
    memcpy(packet, &head, NET_HEAD_LEN);
    memcpy(packet + NET_HEAD_LEN, json_data, json_len);

    net_queue_write(conn, packet, total);
    free(packet);
    return 0;
}

/**
 * @brief Send raw binary data (prepends the 4-byte length header)
 * @return 0 on success, -1 on failure
 */
int net_send_binary(connection_t *conn, const char *data, int len)
{
    if (!conn || !data || conn->closed || len <= 0) return -1;
    if (len > NET_BUF_SIZE - NET_HEAD_LEN) return -1;

    int total = NET_HEAD_LEN + len;
    char *packet = malloc(total);
    uint32_t head = htonl((uint32_t)len);
    memcpy(packet, &head, NET_HEAD_LEN);
    memcpy(packet + NET_HEAD_LEN, data, len);

    net_queue_write(conn, packet, total);
    free(packet);
    return 0;
}

void net_close_connection(connection_t *conn)
{
    if (!conn || conn->closed) return;
    printf("close connection: %s:%d (fd=%d)\n", conn->client_ip, conn->client_port, conn->fd);
    if (conn->nf && conn->nf->ops && conn->nf->ops->close_conn) {
        conn->nf->ops->close_conn(conn);
    }
}

#ifdef USE_IO_URING
#include <liburing.h>
#endif

/**
 * @brief Probe whether the running Linux supports io_uring async IO
 * @return 1 if supported, 0 otherwise
 */
int net_probe_io_uring_support(void)
{
#ifdef USE_IO_URING
    struct io_uring ring;
    int ret = io_uring_queue_init(2, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "[IOURING] probe failed: io_uring_queue_init returned %d (errno=%d: %s)\n",
                ret, errno, strerror(errno));
        return 0;
    }
    io_uring_queue_exit(&ring);
    return 1;
#else
    /* io_uring module not compiled in */
    return 0;
#endif
}

net_framework_t* net_framework_create(int port)
{
    return net_framework_create_with_backend(port, NET_BACKEND_EPOLL);
}

/**
 * @brief Create the framework with the chosen IO backend (epoll / io_uring)
 * @return framework instance, NULL on failure
 */
net_framework_t* net_framework_create_with_backend(int port, net_backend_t backend)
{
    net_framework_t *nf = calloc(1, sizeof(net_framework_t));
    if (!nf) return NULL;
    nf->port = port;
    nf->backend = backend;

    nf->thread_pool = thread_pool_create(NET_THREAD_POOL_SIZE, NET_THREAD_POOL_MAX_TASK);
    if (!nf->thread_pool) { free(nf); return NULL; }

    if (backend == NET_BACKEND_EPOLL) {
        if (nf_epoll_init(nf) != 0) {
            thread_pool_destroy(nf->thread_pool);
            free(nf);
            return NULL;
        }
    }
#ifdef USE_IO_URING
    else {
        if (nf_iouring_init(nf) != 0) {
            thread_pool_destroy(nf->thread_pool);
            free(nf);
            return NULL;
        }
    }
#else
    /* io_uring not compiled in: fall back to epoll */
    else {
        if (nf_epoll_init(nf) != 0) {
            thread_pool_destroy(nf->thread_pool);
            free(nf);
            return NULL;
        }
        nf->backend = NET_BACKEND_EPOLL;
    }
#endif

    printf("network framework created: port %d (%s)\n", port,
           nf->backend == NET_BACKEND_IOURING ? "io_uring" : "epoll");
    return nf;
}

void net_framework_destroy(net_framework_t *nf)
{
    if (!nf) return;
    if (nf->ops && nf->ops->destroy) nf->ops->destroy(nf);
    if (nf->thread_pool) thread_pool_destroy(nf->thread_pool);
    free(nf);
}

int net_framework_start(net_framework_t *nf)
{
    if (!nf || !nf->ops || !nf->ops->start) return -1;
    return nf->ops->start(nf);
}

void net_framework_stop(net_framework_t *nf)
{
    if (!nf || !nf->ops || !nf->ops->stop) return;
    nf->ops->stop(nf);
}

void net_framework_set_callbacks(net_framework_t *nf,
                                 void (*on_accept)(connection_t *conn),
                                 void (*on_recv)(connection_t *conn, const char *data, int len),
                                 void (*on_close)(connection_t *conn),
                                 void (*on_error)(connection_t *conn, int err))
{
    if (!nf) return;
    nf->on_accept = on_accept;
    nf->on_recv = on_recv;
    nf->on_close = on_close;
    nf->on_error = on_error;
}
