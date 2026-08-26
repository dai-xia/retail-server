#ifndef __IOURING_ENGINE_H__
#define __IOURING_ENGINE_H__

#include <linux/io_uring.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef IORING_OP_ACCEPT
#define IORING_OP_ACCEPT        13
#endif
#ifndef IORING_OP_READ
#define IORING_OP_READ          22
#endif
#ifndef IORING_OP_WRITE
#define IORING_OP_WRITE         23
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int ring_fd;

    void *sq_mmap;        /* mmap base of SQ ring */
    void *sqes_mmap;      /* mmap base of SQE array */
    void *cq_mmap;        /* mmap base of CQ ring */
    int sq_mmap_sz;
    int sqes_mmap_sz;
    int cq_mmap_sz;

    /* SQ */
    unsigned *sq_head;
    unsigned *sq_tail;
    unsigned *sq_mask;
    unsigned *sq_entries;
    unsigned *sq_flags;
    unsigned *sq_dropped;
    unsigned *array;
    struct io_uring_sqe *sqes;

    /* CQ */
    unsigned *cq_head;
    unsigned *cq_tail;
    unsigned *cq_mask;
    unsigned *cq_entries;
    unsigned *cq_overflow;
    struct io_uring_cqe *cqes;
    pthread_mutex_t sq_mtx;  /* Protects SQE allocation (thread-safe submit) */
    int sqpoll;              /* 1 = SQPOLL mode, kernel thread polls SQ */
} iouring_t;

iouring_t* iouring_create(unsigned entries, unsigned flags);
void iouring_destroy(iouring_t *r);
int iouring_register_buffers(iouring_t *r, struct iovec *iovecs, unsigned nr_iovecs);

struct io_uring_sqe* iouring_get_sqe(iouring_t *r);
void iouring_sqe_ready(iouring_t *r);

/* In SQPOLL mode, check whether the kernel thread needs manual wakeup */
static inline void iouring_sqe_flush(iouring_t *r)
{
    if (r->sqpoll) {
        /* SQPOLL: kernel thread polls the SQ itself; only enter on NEED_WAKEUP */
        unsigned flags = __atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE);
        if (flags & IORING_SQ_NEED_WAKEUP) {
            (void)syscall(__NR_io_uring_enter, r->ring_fd, 0, 0,
                          IORING_ENTER_SQ_WAKEUP, NULL, 0);
        }
    }
    /* Non-SQPOLL: no-op; caller decides when to iouring_submit */
}

int iouring_submit_and_wait(iouring_t *r, unsigned wait_nr);
int iouring_submit(iouring_t *r);

struct io_uring_cqe* iouring_get_cqe(iouring_t *r);
void iouring_cqe_seen(iouring_t *r, struct io_uring_cqe *cqe);

/* Runtime probe: whether the running kernel supports io_uring.
 * Creates a minimal io_uring instance and closes it immediately.
 * Returns 0 if available, -1 otherwise. */
int iouring_probe(void);

/* io_uring SQE flags - defined here for older kernel headers */
#ifndef IOSQE_FIXED_BUF
#define IOSQE_FIXED_BUF (1U << 4)
#endif

/* io_uring setup flags */
#ifndef IORING_SETUP_SQPOLL
#define IORING_SETUP_SQPOLL (1U << 1)
#endif

/* io_uring SQ ring flags */
#ifndef IORING_SQ_NEED_WAKEUP
#define IORING_SQ_NEED_WAKEUP (1U << 0)
#endif

/* io_uring enter flags */
#ifndef IORING_ENTER_SQ_WAKEUP
#define IORING_ENTER_SQ_WAKEUP (1U << 1)
#endif

/* io_uring register opcodes */
#ifndef IORING_REGISTER_BUFFERS
#define IORING_REGISTER_BUFFERS 4
#endif

#ifdef __cplusplus
}
#endif

#endif
