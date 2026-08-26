/*
 * nf_iouring.c — io_uring network backend (built on liburing)
 *
 * Architecture: main Reactor (ACCEPT) + 4 sub Reactors (READ/WRITE) + thread pool.
 * Uses liburing's standard interface; io_uring_get_sqe() is thread-safe by itself.
 */

#include "nf_iouring.h"
#include <liburing.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stddef.h>
#include <sys/eventfd.h>
#include <errno.h>

/* CQE user_data op tags */
#define UR_OP_READ   0ULL
#define UR_OP_WRITE  1ULL
#define UR_OP_EVENT  2ULL
#define UR_OP_CANCEL 3ULL
#define UR_OP_MASK   (3ULL << 62)

/* io_uring user_data packs [op in top 2 bits][business pointer in low 62 bits].
 * 64-bit Linux virtual addresses only use the low 48 bits, so the top 16 bits
 * are free; we borrow the top 2 to encode the event type alongside the pointer. */
#define UR_MAKE_UD(op, ptr)   ((void *)(uintptr_t)((__u64)(uintptr_t)(ptr) | ((op) << 62)))
#define UR_UD_OP(ud)          ((unsigned)((uintptr_t)(ud) >> 62))
#define UR_UD_PTR(ud)         ((void *)(uintptr_t)((uintptr_t)(ud) & ~(uintptr_t)UR_OP_MASK))

#define IOURING_BUF_POOL_SIZE 512

/* Per-connection io_uring backend data, stored in connection_t->backend_data */
typedef struct {
    void *reactor;               /* owning sub_reactor (iour_sub_t*) */
    int write_sqe_pending;      /* 1 = a WRITE SQE is in flight; suppress duplicate submits */
    int buf_index;              /* fixed-buffer index, -1 = pool unused (fallback to per-conn buffer) */
} conn_iouring_data_t;

typedef struct {
    struct io_uring  ring;          /* liburing instance, owned by this sub thread */
    int              event_fd;     /* eventfd: cross-thread wakeup for the sub Reactor */
    uint64_t         event_buf;
    pthread_t        tid;
    int              index;
    net_framework_t *nf;
    connection_t    *conns[65536];  /* fd -> connection map */
    int              reading[65536]; /* 1 = a READ SQE for this fd is in flight */
    connection_t    *pending_conns[512]; /* new conns handed over by the main thread */
    int              pending_count;
    connection_t    *closed_conns[512];  /* conns to reap */
    int              closed_count;
    connection_t    *retry_conns[256];   /* conns waiting for an SQE slot; resubmitted next round */
    int              retry_count;
    pthread_mutex_t  conn_mtx;     /* protects pending/closed/retry arrays + conns[] */
    int              running;

    /* Fixed-buffer pool */
    struct iovec    *buf_pool;
    int             *free_bufs;     /* stack of free buffer indices */
    int              free_buf_count;
    int              bufs_registered;/* 1 = io_uring_register_buffers succeeded; 0 = fallback */
    pthread_mutex_t  buf_mtx;
} iour_sub_t;

typedef struct {
    int              listen_fd;
    struct io_uring  ring;          /* main reactor ring — ACCEPT only */
    iour_sub_t       subs[NET_SUB_REACTOR_NUM];
} nf_iouring_impl_t;

static void submit_accept(nf_iouring_impl_t *impl, net_framework_t *nf);
static void nf_iouring_submit_write(connection_t *c);
static void nf_iouring_close_conn(connection_t *c);
static void iouring_conn_closed(connection_t *c, iour_sub_t *sr,
                                net_framework_t *nf, int err);
static void iouring_conn_closed_locked(connection_t *c, iour_sub_t *sr,
                                       net_framework_t *nf, int err);
static int  submit_read(iour_sub_t *sr, connection_t *c);
static void enqueue_retry(iour_sub_t *sr, connection_t *c);
static void process_retry(iour_sub_t *sr);
static int  nf_iouring_start(net_framework_t *nf);
static void nf_iouring_stop(net_framework_t *nf);
static void nf_iouring_destroy_impl(net_framework_t *nf);
static int  create_sub_reactor(iour_sub_t *sr, int index, net_framework_t *nf);
static void destroy_sub_reactor(iour_sub_t *sr);

static iour_sub_t *pick_sub(nf_iouring_impl_t *impl)
{
    static volatile int idx = 0;
    int i = __atomic_fetch_add(&idx, 1, __ATOMIC_RELAXED);
    return &impl->subs[i % NET_SUB_REACTOR_NUM];
}

/* Fixed-buffer pool: pre-registered with the kernel so DMA can write directly
 * into the registered region, skipping per-IO address validation. */
static int create_buf_pool(iour_sub_t *sr)
{
    sr->buf_pool = calloc(IOURING_BUF_POOL_SIZE, sizeof(struct iovec));
    sr->free_bufs = calloc(IOURING_BUF_POOL_SIZE, sizeof(int));
    if (!sr->buf_pool || !sr->free_bufs) return -1;

    for (int i = 0; i < IOURING_BUF_POOL_SIZE; i++) {
        sr->buf_pool[i].iov_base = calloc(1, NET_READ_BUF_SIZE);
        if (!sr->buf_pool[i].iov_base) return -1;
        sr->buf_pool[i].iov_len = NET_READ_BUF_SIZE;
        sr->free_bufs[i] = i;
    }
    sr->free_buf_count = IOURING_BUF_POOL_SIZE;
    pthread_mutex_init(&sr->buf_mtx, NULL);

    int ret = io_uring_register_buffers(&sr->ring, sr->buf_pool, IOURING_BUF_POOL_SIZE);
    if (ret < 0) {
        fprintf(stderr, "io_uring register buffers failed (%d), falling back to non-fixed\n", ret);
        sr->bufs_registered = 0;
    } else {
        sr->bufs_registered = 1;
    }
    return 0;
}

static int alloc_buf(iour_sub_t *sr)
{
    pthread_mutex_lock(&sr->buf_mtx);
    if (sr->free_buf_count <= 0) {
        pthread_mutex_unlock(&sr->buf_mtx);
        return -1;
    }
    int idx = sr->free_bufs[--sr->free_buf_count];
    pthread_mutex_unlock(&sr->buf_mtx);
    return idx;
}

static void free_buf(iour_sub_t *sr, int idx)
{
    pthread_mutex_lock(&sr->buf_mtx);
    sr->free_bufs[sr->free_buf_count++] = idx;
    pthread_mutex_unlock(&sr->buf_mtx);
}

static void destroy_buf_pool(iour_sub_t *sr)
{
    if (sr->buf_pool) {
        for (int i = 0; i < IOURING_BUF_POOL_SIZE; i++) {
            free(sr->buf_pool[i].iov_base);
        }
        free(sr->buf_pool);
        sr->buf_pool = NULL;
    }
    free(sr->free_bufs);
    sr->free_bufs = NULL;
    pthread_mutex_destroy(&sr->buf_mtx);
}

/* Submit a READ SQE. Returns 0 on success, -1 otherwise.
 * -1 with c->closed untouched means SQ ring was full; caller retries via enqueue_retry.
 * -1 with c->closed=1 means a hard failure (e.g. buffer overflow) was handled here.
 * Caller must hold sr->conn_mtx (the cap<=0 path calls _locked to avoid recursive lock). */
static int submit_read(iour_sub_t *sr, connection_t *c)
{
    if (c->closed) return -1;

    conn_iouring_data_t *idata = (conn_iouring_data_t *)c->backend_data;
    if (!idata) return -1;

    /* Both fixed-buffer and fallback modes append at c->rlen offset:
     *  - fixed: rbuf_ptr + rlen stays inside the registered iov; the kernel
     *    io_import_fixed() checks pass and DMA writes new bytes after the
     *    leftover, achieving zero-copy. buf_index is exclusive per connection
     *    lifetime, so leftover data is safe to keep there.
     *  - fallback: same rlen offset into the per-connection c->rbuf.
     * In both modes leftover half-frame data is memmove'd to the head by
     * net_parse_frames; no stitch buffer needed. */
    int off = c->rlen;
    int cap = NET_READ_BUF_SIZE - off;
    if (cap <= 0) {
        /* Buffer full but no complete frame: protocol error, close the conn.
         * Caller already holds conn_mtx; use _locked to avoid recursive deadlock. */
        iouring_conn_closed_locked(c, sr, sr->nf, ENOSPC);
        return -1;
    }

    /* io_uring_get_sqe() is thread-safe internally; no external lock needed */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&sr->ring);
    if (!sqe) {
        /* SQ ring full: caller enqueues retry; we don't wake here */
        return -1;
    }

    if (idata->buf_index >= 0 && sr->bufs_registered) {
        io_uring_prep_read_fixed(sqe, c->fd, c->rbuf_ptr + off, cap, 0,
                                 idata->buf_index);
    } else {
        io_uring_prep_read(sqe, c->fd, c->rbuf_ptr + off, cap, 0);
    }
    io_uring_sqe_set_data(sqe, UR_MAKE_UD(UR_OP_READ, c));

    sr->reading[c->fd] = 1;
    net_conn_ref(c);  /* ref for in-flight READ SQE */
    io_uring_submit(&sr->ring);
    return 0;
}

/* Retry queue for SQE-exhausted connections.
 * When submit_read returns -1 due to a full SQ ring, the connection would
 * otherwise be orphaned; we stash it here and resubmit on the next event loop.
 * Callers of enqueue_retry / process_retry must hold sr->conn_mtx.
 * Capacity is 256; if it overflows we close the conn to avoid a leak. */

/* Caller must hold sr->conn_mtx. c must not be closed. */
static void enqueue_retry(iour_sub_t *sr, connection_t *c)
{
    if (c->closed) return;
    if (sr->retry_count >= 256) {
        /* Retry queue also full: fail fast and close. Caller holds conn_mtx,
         * use _locked to avoid recursive deadlock. */
        iouring_conn_closed_locked(c, sr, sr->nf, ENOSPC);
        return;
    }
    sr->retry_conns[sr->retry_count++] = c;
    net_conn_ref(c);  /* retry queue holds a reference */
    /* Wake the sub reactor for the next process_retry round. Even if this
     * call is from within the sub reactor thread (mid for_each_cqe), the
     * write makes io_uring_submit_and_wait return immediately after this round. */
    uint64_t val = 1;
    (void)write(sr->event_fd, &val, sizeof(val));
}

/* Resubmit submit_read for everything in retry_conns.
 * Caller must hold sr->conn_mtx. Successfully submitted conns are removed and
 * unref'd; failures stay for the next round. SQEs free up gradually as CQEs
 * are consumed, so retained conns usually succeed in later rounds. */
static void process_retry(iour_sub_t *sr)
{
    int j = 0;
    for (int i = 0; i < sr->retry_count; i++) {
        connection_t *c = sr->retry_conns[i];
        sr->retry_conns[i] = NULL;
        if (!c || c->closed) {
            /* Closed (possibly cross-thread via nf_iouring_close_conn): drop the retry ref */
            if (c) net_conn_unref(c);
            continue;
        }
        if (submit_read(sr, c) == 0) {
            /* Success: drop the retry-queue ref (submit_read took its own in-flight ref) */
            net_conn_unref(c);
        } else if (c->closed) {
            /* submit_read hit a hard failure (e.g. cap<=0): drop the retry ref */
            net_conn_unref(c);
        } else {
            /* Still SQE-starved: keep for next round */
            sr->retry_conns[j++] = c;
        }
    }
    sr->retry_count = j;
    /* If anything is still pending, wake again so we keep retrying */
    if (j > 0) {
        uint64_t val = 1;
        (void)write(sr->event_fd, &val, sizeof(val));
    }
}

/* Submit a WRITE SQE. May be called from any thread. */
static void nf_iouring_submit_write(connection_t *c)
{
    conn_iouring_data_t *idata = (conn_iouring_data_t *)c->backend_data;
    if (!idata) return;
    iour_sub_t *sr = (iour_sub_t *)idata->reactor;
    if (!sr || c->closed) {
        idata->write_sqe_pending = 0;
        return;
    }

    /* Hold write_lock while grabbing the data pointer and submitting the SQE,
     * so the data can't be freed between unlock and SQE submission */
    pthread_mutex_lock(&c->write_lock);
    write_task_t *wt = c->write_head;
    if (!wt) {
        idata->write_sqe_pending = 0;
        pthread_mutex_unlock(&c->write_lock);
        return;
    }
    int remain = wt->len - wt->offset;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&sr->ring);
    if (!sqe) {
        pthread_mutex_unlock(&c->write_lock);
        idata->write_sqe_pending = 0;
        uint64_t val = 1;
        (void)write(sr->event_fd, &val, sizeof(val));
        return;
    }

    io_uring_prep_write(sqe, c->fd, wt->data + wt->offset, remain, (__u64)-1);
    io_uring_sqe_set_data(sqe, UR_MAKE_UD(UR_OP_WRITE, c));

    net_conn_ref(c);  /* ref for in-flight WRITE SQE */
    pthread_mutex_unlock(&c->write_lock);
    io_uring_submit(&sr->ring);
}

/* Lock-acquiring variant of iouring_conn_closed_locked. */
static void iouring_conn_closed(connection_t *c, iour_sub_t *sr,
                                 net_framework_t *nf, int err)
{
    pthread_mutex_lock(&sr->conn_mtx);
    iouring_conn_closed_locked(c, sr, nf, err);
    pthread_mutex_unlock(&sr->conn_mtx);
}

/* Caller already holds sr->conn_mtx. Same logic as iouring_conn_closed but
 * skips locking, to avoid recursive deadlock from enqueue_retry/process_retry. */
static void iouring_conn_closed_locked(connection_t *c, iour_sub_t *sr,
                                       net_framework_t *nf, int err)
{
    c->closed = 1;

    if (err > 0 && nf->on_error) nf->on_error(c, err);

    close(c->fd);

    if (c->fd >= 0 && c->fd < 65536 && sr->conns[c->fd] == c) {
        sr->conns[c->fd] = NULL;
    }
    sr->reading[c->fd] = 0;

    conn_iouring_data_t *idata = (conn_iouring_data_t *)c->backend_data;
    if (idata && idata->buf_index >= 0) {
        free_buf(sr, idata->buf_index);
        idata->buf_index = -1;
        c->rbuf_ptr = NULL;
    }

    if (nf->on_close) nf->on_close(c);
    net_conn_unref(c);
}

/* io_uring close connection. May be called from any thread. */
static void nf_iouring_close_conn(connection_t *c)
{
    if (c->closed) return;
    c->closed = 1;

    conn_iouring_data_t *idata = (conn_iouring_data_t *)c->backend_data;
    if (!idata) {
        close(c->fd);
        if (c->nf && c->nf->on_close) c->nf->on_close(c);
        net_conn_unref(c);
        return;
    }

    iour_sub_t *sr = (iour_sub_t *)idata->reactor;
    idata->write_sqe_pending = 0;

    /* Cancel any in-flight READ SQE so a reused fd doesn't read stale data */
    if (sr && c->fd >= 0 && c->fd < 65536 && sr->reading[c->fd]) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&sr->ring);
        if (sqe) {
            io_uring_prep_cancel(sqe, UR_MAKE_UD(UR_OP_READ, c), 0);
            io_uring_sqe_set_data(sqe, UR_MAKE_UD(UR_OP_CANCEL, NULL));
            io_uring_submit(&sr->ring);
        }
    }

    /* Return the fixed buffer to the pool */
    if (idata->buf_index >= 0 && sr) {
        free_buf(sr, idata->buf_index);
        idata->buf_index = -1;
    }

    /* Submit the CANCEL SQE before close(fd); we don't wait for the cancel
     * to take effect. */
    close(c->fd);

    /* Add to closed_conns so the sub reactor can clear conns[] */
    if (sr) {
        pthread_mutex_lock(&sr->conn_mtx);
        if (sr->closed_count < 512) {
            sr->closed_conns[sr->closed_count++] = c;
        }
        pthread_mutex_unlock(&sr->conn_mtx);

        uint64_t val = 1;
        (void)write(sr->event_fd, &val, sizeof(val));
    }

    if (c->nf && c->nf->on_close) c->nf->on_close(c);
}

/* Sub Reactor worker thread */
static void *iour_sub_thread(void *arg)
{
    iour_sub_t *sr = (iour_sub_t *)arg;
    net_framework_t *nf = sr->nf;

    while (__atomic_load_n(&sr->running, __ATOMIC_ACQUIRE)) {
        int ret = io_uring_submit_and_wait(&sr->ring, 1);
        if (ret < 0 && ret != -EINTR) break;

        struct io_uring_cqe *cqe;
        unsigned head;
        unsigned cqe_count = 0;

        /* Batch-process all ready CQEs (liburing-recommended) */
        io_uring_for_each_cqe(&sr->ring, head, cqe) {
            cqe_count++;
            unsigned op = UR_UD_OP(cqe->user_data);
            void *ptr = UR_UD_PTR(cqe->user_data);

            switch (op) {
            case UR_OP_EVENT: {       /* eventfd wakeup: new conns from main thread / external closes */
                pthread_mutex_lock(&sr->conn_mtx);
                for (int i = 0; i < sr->pending_count; i++) {
                    connection_t *pc = sr->pending_conns[i];
                    sr->pending_conns[i] = NULL;
                    if (pc && !pc->closed) {
                        if (submit_read(sr, pc) != 0 && !pc->closed) {
                            /* SQ ring full: enqueue for retry. enqueue_retry
                             * takes its own ref, cancelling out the unref below. */
                            enqueue_retry(sr, pc);
                        }
                    }
                    net_conn_unref(pc);
                }
                sr->pending_count = 0;

                for (int i = 0; i < sr->closed_count; i++) {
                    connection_t *cc = sr->closed_conns[i];
                    sr->closed_conns[i] = NULL;
                    if (cc && cc->fd >= 0 && cc->fd < 65536 && sr->conns[cc->fd] == cc) {
                        sr->conns[cc->fd] = NULL;
                        sr->reading[cc->fd] = 0;
                    }
                    net_conn_unref(cc);
                }
                sr->closed_count = 0;
                pthread_mutex_unlock(&sr->conn_mtx);

                /* Resubmit the eventfd READ */
                struct io_uring_sqe *esqe = io_uring_get_sqe(&sr->ring);
                if (esqe) {
                    io_uring_prep_read(esqe, sr->event_fd, &sr->event_buf, 8, 0);
                    io_uring_sqe_set_data(esqe, UR_MAKE_UD(UR_OP_EVENT,
                        (void *)(uintptr_t)(long)sr->event_fd));
                    io_uring_submit(&sr->ring);
                } else {
                    uint64_t val = 1;
                    (void)write(sr->event_fd, &val, sizeof(val));
                }
                break;
            }

            case UR_OP_READ: {
                connection_t *c = (connection_t *)ptr;
                conn_iouring_data_t *ridata = (conn_iouring_data_t *)c->backend_data;
                sr->reading[c->fd] = 0;

                if (c->closed) {
                    if (ridata && ridata->buf_index >= 0 && sr->bufs_registered) {
                        free_buf(sr, ridata->buf_index);
                        ridata->buf_index = -1;
                    }
                    net_conn_unref(c);
                    break;
                }

                if (cqe->res <= 0) {
                    int err = (cqe->res < 0) ? -cqe->res : ECONNRESET;
                    iouring_conn_closed(c, sr, nf, err);
                    net_conn_unref(c);
                } else {
                    /* submit_read made the kernel write new bytes at rbuf_ptr + rlen,
                     * so rbuf_ptr[0 .. rlen + cqe->res) is a continuous [leftover|new]
                     * stream. net_parse_frames appends bytes_read and loops over
                     * frames, then memmoves any unconsumed leftover back to the head
                     * for the next read to continue. */
                    int parse_ret = net_parse_frames(c, nf, cqe->res);

                    if (parse_ret < 0 || c->closed) {
                        if (!c->closed) {
                            iouring_conn_closed(c, sr, nf, EBADMSG);
                        }
                        net_conn_unref(c);
                    } else {
                        /* submit_read requires holding conn_mtx (the cap<=0 path
                         * calls _locked). On SQ ring full, enqueue_retry takes a
                         * ref that cancels out the unref below (old in-flight ref). */
                        pthread_mutex_lock(&sr->conn_mtx);
                        if (submit_read(sr, c) != 0 && !c->closed) {
                            enqueue_retry(sr, c);
                        }
                        pthread_mutex_unlock(&sr->conn_mtx);
                        net_conn_unref(c);
                    }
                }
                break;
            }

            case UR_OP_WRITE: {
                connection_t *c = (connection_t *)ptr;
                conn_iouring_data_t *widata = (conn_iouring_data_t *)c->backend_data;
                if (widata) widata->write_sqe_pending = 0;

                if (c->closed) {
                    net_conn_unref(c);
                    break;
                }

                if (cqe->res < 0) {
                    iouring_conn_closed(c, sr, nf, -cqe->res);
                    net_conn_unref(c);
                } else {
                    pthread_mutex_lock(&c->write_lock);
                    write_task_t *wt = c->write_head;
                    if (wt) {
                        wt->offset += cqe->res;
                        if (wt->offset >= wt->len) {
                            c->write_head = wt->next;
                            if (!c->write_head) c->write_tail = NULL;
                            c->write_count--;
                            free(wt->data);
                            free(wt);
                        }
                    }
                    /* If more data is queued, submit the next WRITE SQE */
                    if (c->write_head && !c->closed) {
                        widata->write_sqe_pending = 1;
                        write_task_t *nwt = c->write_head;
                        int nremain = nwt->len - nwt->offset;

                        struct io_uring_sqe *wsqe = io_uring_get_sqe(&sr->ring);
                        if (wsqe) {
                            io_uring_prep_write(wsqe, c->fd,
                                nwt->data + nwt->offset, nremain, (__u64)-1);
                            io_uring_sqe_set_data(wsqe, UR_MAKE_UD(UR_OP_WRITE, c));
                            net_conn_ref(c);
                            io_uring_submit(&sr->ring);
                        } else {
                            widata->write_sqe_pending = 0;
                        }
                    }
                    pthread_mutex_unlock(&c->write_lock);
                    net_conn_unref(c);
                }
                break;
            }

            case UR_OP_CANCEL:
                /* ASYNC_CANCEL CQE — no action; the cancelled READ SQE produces
                 * its own CQE (res=-ECANCELED) handled by UR_OP_READ */
                break;
            }
        }
        io_uring_cq_advance(&sr->ring, cqe_count);

        /* This round's CQEs are done and some SQEs have been reclaimed by the
         * kernel. Retry SQE-starved conns to try to flush them this round. */
        pthread_mutex_lock(&sr->conn_mtx);
        process_retry(sr);
        pthread_mutex_unlock(&sr->conn_mtx);
    }

    return NULL;
}

/* Normally the new connection just goes into pending_conns; if the queue is
 * already full (512), we submit the read directly. */
static void assign_connection(nf_iouring_impl_t *impl, connection_t *c, net_framework_t *nf)
{
    iour_sub_t *sr = pick_sub(impl);

    int buf_idx = alloc_buf(sr);

    conn_iouring_data_t *idata = calloc(1, sizeof(conn_iouring_data_t));
    idata->reactor = sr;
    idata->write_sqe_pending = 0;
    idata->buf_index = buf_idx;
    /* buf_idx < 0 means the pool is exhausted (all 512 in use); fall back to
     * the per-connection buffer */

    c->backend = NET_BACKEND_IOURING;
    c->backend_data = idata;
    c->nf = nf;

    if (idata->buf_index >= 0 && sr->bufs_registered) {
        c->rbuf_ptr = sr->buf_pool[idata->buf_index].iov_base;
    } else {
        c->rbuf_ptr = c->rbuf;
    }

    pthread_mutex_lock(&sr->conn_mtx);
    if (c->fd >= 0 && c->fd < 65536) {
        if (sr->conns[c->fd] && sr->conns[c->fd] != c) {
            net_conn_unref(sr->conns[c->fd]);
        }
        sr->conns[c->fd] = c;
        if (sr->pending_count < 512) {
            sr->pending_conns[sr->pending_count++] = c;
            net_conn_ref(c);
        } else {
            fprintf(stderr, "io_uring: pending_conns full, submitting read directly fd=%d\n", c->fd);
            if (submit_read(sr, c) != 0 && !c->closed) {
                /* SQ ring full (already holding conn_mtx): enqueue for retry */
                enqueue_retry(sr, c);
            }
        }
    }
    pthread_mutex_unlock(&sr->conn_mtx);

    uint64_t val = 1;
    (void)write(sr->event_fd, &val, sizeof(val));
}

/* Init one sub Reactor: io_uring queue, eventfd, conn queues, fixed-buffer
 * pool, pre-submit the eventfd read, then spawn the worker thread. */
static int create_sub_reactor(iour_sub_t *sr, int index, net_framework_t *nf)
{
    /* SQPOLL: a kernel thread polls the SQ automatically, cutting userspace
     * submit syscalls and boosting high-concurrency throughput. Fall back to
     * normal mode if unsupported (old kernel / container / permissions). */
    if (io_uring_queue_init(1024, &sr->ring, IORING_SETUP_SQPOLL) < 0) {
        if (io_uring_queue_init(1024, &sr->ring, 0) < 0)
            return -1;
    }

    sr->event_fd = eventfd(0, EFD_NONBLOCK);
    if (sr->event_fd < 0) {
        io_uring_queue_exit(&sr->ring);
        return -1;
    }

    sr->event_buf = 0;
    sr->index = index;
    sr->nf = nf;
    sr->running = 1;
    sr->pending_count = 0;
    sr->closed_count = 0;

    pthread_mutex_init(&sr->conn_mtx, NULL);

    /* Fixed-buffer pool (512 buffers) for read_fixed zero-copy. On failure the
     * framework degrades to per-connection buffers automatically. */
    if (create_buf_pool(sr) < 0) {
        fprintf(stderr, "Warning: fixed buffer pool creation failed, using fallback\n");
    }

    /* Pre-submit the eventfd read so the sub thread can be woken up */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&sr->ring);
    if (sqe) {
        io_uring_prep_read(sqe, sr->event_fd, &sr->event_buf, 8, 0);
        io_uring_sqe_set_data(sqe, UR_MAKE_UD(UR_OP_EVENT,
            (void *)(uintptr_t)(long)sr->event_fd));
    }
    io_uring_submit(&sr->ring);

    return pthread_create(&sr->tid, NULL, iour_sub_thread, sr);
}

static void destroy_sub_reactor(iour_sub_t *sr)
{
    __atomic_store_n(&sr->running, 0, __ATOMIC_RELEASE);
    uint64_t val = 1;
    (void)write(sr->event_fd, &val, sizeof(val));
    pthread_join(sr->tid, NULL);

    for (int i = 0; i < 65536; i++) {
        if (sr->conns[i]) {
            net_conn_unref(sr->conns[i]);
            sr->conns[i] = NULL;
        }
    }

    /* Drain leftover retry-queue refs (sub thread joined, no concurrency) */
    for (int i = 0; i < sr->retry_count; i++) {
        if (sr->retry_conns[i]) {
            net_conn_unref(sr->retry_conns[i]);
            sr->retry_conns[i] = NULL;
        }
    }
    sr->retry_count = 0;

    destroy_buf_pool(sr);

    if (sr->event_fd >= 0) close(sr->event_fd);
    io_uring_queue_exit(&sr->ring);
    pthread_mutex_destroy(&sr->conn_mtx);
}

static void submit_accept(nf_iouring_impl_t *impl, net_framework_t *nf)
{
    (void)nf;
    struct io_uring_sqe *sqe = io_uring_get_sqe(&impl->ring);
    if (!sqe) return;

    io_uring_prep_accept(sqe, impl->listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, 0);  /* ACCEPT uses user_data=0 */
}

/* Main Reactor thread: only handles listen + accept, dispatching new conns
 * to the sub Reactors. Sub Reactors handle READ/WRITE and reaping. */
static void *iouring_main_thread(void *arg)
{
    net_framework_t *nf = (net_framework_t *)arg;
    nf_iouring_impl_t *impl = (nf_iouring_impl_t *)nf->impl;

    while (__atomic_load_n(&nf->running, __ATOMIC_ACQUIRE))
    {
        int ret = io_uring_submit_and_wait(&impl->ring, 1);
        if (ret < 0 && ret != -EINTR)
            break;

        struct io_uring_cqe *cqe;
        unsigned head;
        unsigned cqe_count = 0;

        io_uring_for_each_cqe(&impl->ring, head, cqe)
        {
            cqe_count++;

            if (cqe->res < 0)
            {
                /* accept failed (listen fd error, queue full, ...); resubmit */
                submit_accept(impl, nf);
                continue;
            }

            int client_fd = cqe->res;
            struct sockaddr_in addr;
            socklen_t addrlen = sizeof(addr);

            getpeername(client_fd, (struct sockaddr *)&addr, &addrlen);

            net_set_nonblocking(client_fd);

            connection_t *c = calloc(1, sizeof(connection_t));
            c->fd = client_fd;
            c->ref = 1;
            c->nf = nf;
            inet_ntop(AF_INET, &addr.sin_addr, c->client_ip, sizeof(c->client_ip));
            c->client_port = ntohs(addr.sin_port);
            c->last_active = time(NULL);
            pthread_mutex_init(&c->write_lock, NULL);

            assign_connection(impl, c, nf);

            if (nf->on_accept)
                nf->on_accept(c);

            /* Resubmit so we keep listening for the next conn */
            submit_accept(impl, nf);
        }

        io_uring_cq_advance(&impl->ring, cqe_count);
    }

    return NULL;
}

static const net_framework_ops_t iouring_ops = {
    .destroy      = nf_iouring_destroy_impl,
    .start        = nf_iouring_start,
    .stop         = nf_iouring_stop,
    .submit_write = nf_iouring_submit_write,
    .close_conn   = nf_iouring_close_conn,
};

int nf_iouring_init(net_framework_t *nf)
{
    nf_iouring_impl_t *impl = calloc(1, sizeof(nf_iouring_impl_t));
    if (!impl) return -1;
    nf->impl = impl;
    nf->ops = &iouring_ops;
    return 0;
}


static int nf_iouring_start(net_framework_t *nf)
{
    nf_iouring_impl_t *impl = (nf_iouring_impl_t *)nf->impl;

    impl->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (impl->listen_fd < 0) return -1;

    int opt = 1;
    /* SO_REUSEADDR: rebind immediately on restart, avoiding "Address already in use" */
    setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* SO_REUSEPORT: let multiple threads/processes listen on the same port;
     * the kernel load-balances accepts, fitting the multi-Reactor design */
    setsockopt(impl->listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    /* TCP_NODELAY: disable Nagle so small packets go out immediately (low latency) */
    setsockopt(impl->listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(nf->port);

    if (bind(impl->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(impl->listen_fd);
        return -1;
    }
    if (listen(impl->listen_fd, 512) < 0) {
        close(impl->listen_fd);
        return -1;
    }
    net_set_nonblocking(impl->listen_fd);

    /* Main reactor ring (no SQPOLL; ACCEPT only) */
    if (io_uring_queue_init(256, &impl->ring, 0) < 0) {
        close(impl->listen_fd);
        return -1;
    }

    /* Submit the initial ACCEPT SQE */
    submit_accept(impl, nf);
    io_uring_submit(&impl->ring);

    /* Create the sub Reactors */
    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        if (create_sub_reactor(&impl->subs[i], i, nf) != 0) {
            for (int j = 0; j < i; j++) destroy_sub_reactor(&impl->subs[j]);
            io_uring_queue_exit(&impl->ring);
            close(impl->listen_fd);
            return -1;
        }
    }

    nf->running = 1;

    pthread_t main_tid;
    pthread_create(&main_tid, NULL, iouring_main_thread, nf);
    pthread_detach(main_tid);

    return 0;
}

static void nf_iouring_stop(net_framework_t *nf)
{
    nf->running = 0;
    nf_iouring_impl_t *impl = (nf_iouring_impl_t *)nf->impl;

    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        __atomic_store_n(&impl->subs[i].running, 0, __ATOMIC_RELEASE);
        uint64_t val = 1;
        (void)write(impl->subs[i].event_fd, &val, sizeof(val));
        pthread_join(impl->subs[i].tid, NULL);
    }
}

static void nf_iouring_destroy_impl(net_framework_t *nf)
{
    nf_iouring_impl_t *impl = (nf_iouring_impl_t *)nf->impl;
    if (!impl) return;

    for (int i = 0; i < NET_SUB_REACTOR_NUM; i++) {
        destroy_sub_reactor(&impl->subs[i]);
    }

    if (impl->listen_fd >= 0) close(impl->listen_fd);
    io_uring_queue_exit(&impl->ring);

    free(impl);
    nf->impl = NULL;
}
