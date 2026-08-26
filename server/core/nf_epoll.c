/* nf_epoll.c — epoll backend implementation
 * Architecture: main Reactor + sub Reactors + edge-triggered epoll (ET) + thread pool
 */
#include "nf_epoll.h"
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <errno.h>

/* One sub Reactor per epoll event thread; handles read/write on connected sockets */
typedef struct {
    int epfd;
    int event_fd;                /* eventfd: wakes up epoll_wait for inter-thread notification */
    pthread_t tid;
    int index;
    net_framework_t *nf;
    connection_t *conns[65536];  /* fd -> connection_t map (max fd 65535) */
    pthread_mutex_t conn_mtx;
    int running;
} sub_reactor_t;

/* Holds the listen fd, main epoll, and all sub Reactors */
typedef struct {
    int listen_fd;
    int main_epfd;
    sub_reactor_t sub_reactors[NET_SUB_REACTOR_NUM];
} nf_epoll_impl_t;

/* Per-connection epoll private data, stored in connection_t->backend_data */
typedef struct {
    void *reactor;    /* owning sub_reactor_t* */
    int events;       /* current epoll event mask (EPOLLIN/EPOLLOUT) */
} conn_epoll_data_t;

static void epoll_submit_write(connection_t *c);
static void epoll_close_conn(connection_t *c);
static void epoll_destroy(net_framework_t *nf);
static int  epoll_start(net_framework_t *nf);
static void epoll_stop(net_framework_t *nf);

/* vtable: the upper framework only calls ops, agnostic to epoll vs io_uring */
static const net_framework_ops_t epoll_ops = {
    .destroy      = epoll_destroy,
    .start        = epoll_start,
    .stop         = epoll_stop,
    .submit_write = epoll_submit_write,
    .close_conn   = epoll_close_conn,
};

int nf_epoll_init(net_framework_t *nf)
{
    nf_epoll_impl_t *impl = calloc(1, sizeof(nf_epoll_impl_t));
    if (!impl) return -1;

    nf->impl = impl;
    nf->ops = &epoll_ops;
    return 0;
}

/* Round-robin a sub Reactor for load balancing across client connections */
static sub_reactor_t *pick_sub_reactor(nf_epoll_impl_t *impl)
{
    static volatile int idx = 0;
    /* RELAXED is fine here: we only need atomic counter increment, no ordering */
    int i = __atomic_fetch_add(&idx, 1, __ATOMIC_RELAXED);
    return &impl->sub_reactors[i % NET_SUB_REACTOR_NUM];
}

static void epoll_close_conn(connection_t *c)
{
    if (c->closed) return;
    c->closed = 1;

    conn_epoll_data_t *edata = (conn_epoll_data_t *)c->backend_data;
    if (edata) {
        sub_reactor_t *sr = (sub_reactor_t *)edata->reactor;
        if (sr) {
            epoll_ctl(sr->epfd, EPOLL_CTL_DEL, c->fd, NULL);

            pthread_mutex_lock(&sr->conn_mtx);
            if (sr->conns[c->fd] == c) sr->conns[c->fd] = NULL;
            pthread_mutex_unlock(&sr->conn_mtx);
        }
    }

    close(c->fd);

    if (c->nf && c->nf->on_close) c->nf->on_close(c);

    net_conn_unref(c);
}

/* When data is pending, flip on EPOLLOUT so epoll drives the send */
static void epoll_submit_write(connection_t *c)
{
    conn_epoll_data_t *edata = (conn_epoll_data_t *)c->backend_data;
    if (!edata) return;
    sub_reactor_t *sr = (sub_reactor_t *)edata->reactor;
    if (!sr) return;

    if (!(edata->events & EPOLLOUT)) {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = c;
        epoll_ctl(sr->epfd, EPOLL_CTL_MOD, c->fd, &ev);
        edata->events = EPOLLIN | EPOLLOUT | EPOLLET;
    }
}

/* Sub Reactor worker thread: loops epoll_wait, handles read/write/error events */
static void *epoll_sub_thread(void *arg)
{
    sub_reactor_t *sr = (sub_reactor_t *)arg;
    net_framework_t *nf = sr->nf;
    struct epoll_event events[NET_MAX_EVENTS];

    while (__atomic_load_n(&sr->running, __ATOMIC_ACQUIRE)) {
        int nfds = epoll_wait(sr->epfd, events, NET_MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            /* event_fd wakeup (data.ptr == NULL marker) */
            if (events[i].data.ptr == NULL) {
                uint64_t val;
                (void)read(sr->event_fd, &val, sizeof(val));
                continue;
            }

            connection_t *c = (connection_t *)events[i].data.ptr;
            if (!c || c->closed) continue;

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                int err = (events[i].events & EPOLLERR) ? ECONNRESET : EPIPE;
                if (nf->on_error) nf->on_error(c, err);
                epoll_close_conn(c);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                int read_ok = 1;
                /* ET mode: must drain the socket buffer in one go */
                while (1) {
                    int nread = read(c->fd, c->rbuf_ptr + c->rlen,
                                     NET_READ_BUF_SIZE - c->rlen);
                    if (nread <= 0) {
                        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            break;
                        if (nf->on_error) nf->on_error(c, nread < 0 ? errno : ECONNRESET);
                        epoll_close_conn(c);
                        read_ok = 0;
                        break;
                    }
                    if (net_parse_frames(c, nf, nread) != 0) {
                        if (nf->on_error) nf->on_error(c, EBADMSG);
                        epoll_close_conn(c);
                        read_ok = 0;
                        break;
                    }
                }
                if (!read_ok) continue;
            }

            if (!c->closed && (events[i].events & EPOLLOUT)) {
                pthread_mutex_lock(&c->write_lock);
                write_task_t *wt = c->write_head;

                while (wt) {
                    int remain = wt->len - wt->offset;
                    /* MSG_NOSIGNAL suppresses SIGPIPE on closed peers */
                    int nsent = send(c->fd, wt->data + wt->offset, remain,
                                     MSG_NOSIGNAL);
                    if (nsent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        pthread_mutex_unlock(&c->write_lock);
                        if (nf->on_error) nf->on_error(c, errno);
                        epoll_close_conn(c);
                        continue;
                    }
                    wt->offset += nsent;
                    if (wt->offset >= wt->len) {
                        c->write_head = wt->next;
                        if (!c->write_head) c->write_tail = NULL;
                        c->write_count--;
                        free(wt->data);
                        free(wt);
                        wt = c->write_head;
                    } else {
                        /* Not fully sent: wait for the next EPOLLOUT */
                        break;
                    }
                }

                /* Queue empty: drop EPOLLOUT to avoid busy-looping */
                if (!c->write_head) {
                    conn_epoll_data_t *ed = (conn_epoll_data_t *)c->backend_data;
                    if (ed) {
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.ptr = c;
                        epoll_ctl(sr->epfd, EPOLL_CTL_MOD, c->fd, &ev);
                        ed->events = EPOLLIN | EPOLLET;
                    }
                }
                pthread_mutex_unlock(&c->write_lock);
            }
        }
    }
    return NULL;
}

/* Accept a new client fd, set up the connection and attach it to a sub Reactor */
static void assign_connection(nf_epoll_impl_t *impl, int cfd,
                              struct sockaddr_in *addr, net_framework_t *nf)
{
    net_set_nonblocking(cfd);

    if (cfd < 0 || cfd >= 65536) {
        close(cfd);
        return;
    }

    sub_reactor_t *sr = pick_sub_reactor(impl);

    connection_t *c = calloc(1, sizeof(connection_t));
    c->fd = cfd;
    c->ref = 1;
    c->backend = NET_BACKEND_EPOLL;
    c->nf = nf;
    c->rbuf_ptr = c->rbuf;
    inet_ntop(AF_INET, &addr->sin_addr, c->client_ip, sizeof(c->client_ip));
    c->client_port = ntohs(addr->sin_port);
    c->last_active = time(NULL);
    pthread_mutex_init(&c->write_lock, NULL);

    conn_epoll_data_t *edata = calloc(1, sizeof(conn_epoll_data_t));
    edata->reactor = sr;
    edata->events = EPOLLIN | EPOLLET;
    c->backend_data = edata;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = c;
    if (epoll_ctl(sr->epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
        perror("epoll_ctl ADD conn");
        free(edata);
        c->backend_data = NULL;
        close(cfd);
        pthread_mutex_destroy(&c->write_lock);
        free(c);
        return;
    }

    pthread_mutex_lock(&sr->conn_mtx);
    if (sr->conns[cfd] && sr->conns[cfd] != c) {
        net_conn_unref(sr->conns[cfd]);
    }
    sr->conns[cfd] = c;
    pthread_mutex_unlock(&sr->conn_mtx);

    uint64_t val = 1;
    (void)write(sr->event_fd, &val, sizeof(val));

    if (nf->on_accept) nf->on_accept(c);
}

/* Main Reactor thread: only listens on listen_fd and accepts new clients */
static void *epoll_main_thread(void *arg)
{
    net_framework_t *nf = (net_framework_t *)arg;
    nf_epoll_impl_t *impl = (nf_epoll_impl_t *)nf->impl;
    struct epoll_event events[NET_MAX_EVENTS];

    while (__atomic_load_n(&nf->running, __ATOMIC_ACQUIRE)) {
        int nfds = epoll_wait(impl->main_epfd, events, NET_MAX_EVENTS, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("main epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == impl->listen_fd) {
                /* ET: drain the accept queue in one go */
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t addr_len = sizeof(client_addr);
                    int cfd = accept(impl->listen_fd,
                                     (struct sockaddr *)&client_addr, &addr_len);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept failed");
                        break;
                    }
                    assign_connection(impl, cfd, &client_addr, nf);
                }
            }
        }
    }

    printf("main Reactor event loop exited\n");
    return NULL;
}

static int epoll_start(net_framework_t *nf)
{
    nf_epoll_impl_t *impl = (nf_epoll_impl_t *)nf->impl;

    /* Block SIGPIPE so send() on a closed peer doesn't kill the process */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    impl->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (impl->listen_fd < 0) { perror("socket failed"); return -1; }

    int opt = 1;
    setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(nf->port);

    if (bind(impl->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed"); close(impl->listen_fd); impl->listen_fd = -1; return -1;
    }

    net_set_nonblocking(impl->listen_fd);

    if (listen(impl->listen_fd, 512) < 0) {
        perror("listen failed"); close(impl->listen_fd); impl->listen_fd = -1; return -1;
    }

    impl->main_epfd = epoll_create1(0);
    if (impl->main_epfd < 0) {
        perror("epoll_create1 failed"); close(impl->listen_fd); impl->listen_fd = -1; return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = impl->listen_fd;
    if (epoll_ctl(impl->main_epfd, EPOLL_CTL_ADD, impl->listen_fd, &ev) < 0) {
        perror("epoll_ctl add listen_fd failed");
        close(impl->main_epfd); close(impl->listen_fd);
        impl->main_epfd = -1; impl->listen_fd = -1; return -1;
    }

    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        sub_reactor_t *sr = &impl->sub_reactors[i];
        sr->epfd = epoll_create1(0);
        sr->event_fd = eventfd(0, EFD_NONBLOCK);
        sr->index = i;
        sr->nf = nf;
        sr->running = 1;
        pthread_mutex_init(&sr->conn_mtx, NULL);

        struct epoll_event eev;
        eev.events = EPOLLIN;
        eev.data.ptr = NULL;
        epoll_ctl(sr->epfd, EPOLL_CTL_ADD, sr->event_fd, &eev);

        pthread_create(&sr->tid, NULL, epoll_sub_thread, sr);
    }

    nf->running = 1;

    printf("network framework started: main Reactor + %d sub Reactors + %d thread pool\n",
           NET_SUB_REACTOR_NUM, NET_THREAD_POOL_SIZE);

    pthread_t main_tid;
    pthread_create(&main_tid, NULL, epoll_main_thread, nf);
    pthread_detach(main_tid);

    return 0;
}

static void epoll_stop(net_framework_t *nf)
{
    nf->running = 0;
    nf_epoll_impl_t *impl = (nf_epoll_impl_t *)nf->impl;
    if (!impl) return;

    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        sub_reactor_t *sr = &impl->sub_reactors[i];
        __atomic_store_n(&sr->running, 0, __ATOMIC_RELEASE);
        /* Wake any blocked epoll_wait */
        uint64_t val = 1;
        (void)write(sr->event_fd, &val, sizeof(val));
        pthread_join(sr->tid, NULL);
    }
}

static void epoll_destroy(net_framework_t *nf)
{
    nf_epoll_impl_t *impl = (nf_epoll_impl_t *)nf->impl;
    if (!impl) return;

    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        sub_reactor_t *sr = &impl->sub_reactors[i];
        for (int j = 0; j < 65536; j++) {
            if (sr->conns[j]) {
                net_conn_unref(sr->conns[j]);
                sr->conns[j] = NULL;
            }
        }
        if (sr->event_fd >= 0) close(sr->event_fd);
        if (sr->epfd >= 0) close(sr->epfd);
        pthread_mutex_destroy(&sr->conn_mtx);
    }

    if (impl->listen_fd >= 0) close(impl->listen_fd);
    if (impl->main_epfd >= 0) close(impl->main_epfd);

    free(impl);
    nf->impl = NULL;
    printf("epoll network framework destroyed\n");
}
