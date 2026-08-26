#include "iouring_engine.h"
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_enter(int ring_fd, unsigned to_submit,
                          unsigned min_complete, unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit,
                        min_complete, flags, NULL, 0);
}

int iouring_probe(void)
{
    /* Try to create a minimal io_uring instance via syscall, then close it.
     * Kernels < 5.1 lack __NR_io_uring_setup and won't compile; if the
     * Linux headers are new enough but the runtime kernel is too old,
     * the syscall returns -ENOSYS. */
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = (int)syscall(__NR_io_uring_setup, 2, &p);
    if (fd < 0) {
        fprintf(stderr, "[IOURING] probe failed: io_uring_setup returned %d (errno=%d: %s)\n",
                fd, errno, strerror(errno));
        return -1;
    }
    close(fd);
    return 0;
}

iouring_t* iouring_create(unsigned entries, unsigned flags)
{
    iouring_t *r = calloc(1, sizeof(iouring_t));
    if (!r) return NULL;

    pthread_mutex_init(&r->sq_mtx, NULL);

    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    p.flags = flags;

    r->ring_fd = io_uring_setup(entries, &p);
    if (r->ring_fd < 0) {
        /* SQPOLL may fail due to permissions/kernel version; fall back to normal mode */
        if (flags & IORING_SETUP_SQPOLL) {
            memset(&p, 0, sizeof(p));
            p.flags = flags & ~IORING_SETUP_SQPOLL;
            r->ring_fd = io_uring_setup(entries, &p);
        }
        if (r->ring_fd < 0) { free(r); return NULL; }
        r->sqpoll = 0;
    } else {
        r->sqpoll = (flags & IORING_SETUP_SQPOLL) ? 1 : 0;
    }

    /* --- SQ ring --- */
    r->sq_mmap_sz = (int)(p.sq_off.array + p.sq_entries * sizeof(unsigned));
    r->sq_mmap = mmap(NULL, (size_t)r->sq_mmap_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
    if (r->sq_mmap == MAP_FAILED) goto err;

    r->sq_head    = (unsigned *)((char *)r->sq_mmap + p.sq_off.head);
    r->sq_tail    = (unsigned *)((char *)r->sq_mmap + p.sq_off.tail);
    r->sq_mask    = (unsigned *)((char *)r->sq_mmap + p.sq_off.ring_mask);
    r->sq_entries = (unsigned *)((char *)r->sq_mmap + p.sq_off.ring_entries);
    r->sq_flags   = (unsigned *)((char *)r->sq_mmap + p.sq_off.flags);
    r->sq_dropped = (unsigned *)((char *)r->sq_mmap + p.sq_off.dropped);
    r->array      = (unsigned *)((char *)r->sq_mmap + p.sq_off.array);

    /* --- SQE array --- */
    r->sqes_mmap_sz = (int)(p.sq_entries * sizeof(struct io_uring_sqe));
    r->sqes_mmap = mmap(NULL, (size_t)r->sqes_mmap_sz, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQES);
    if (r->sqes_mmap == MAP_FAILED) goto err_sq;
    r->sqes = (struct io_uring_sqe *)r->sqes_mmap;

    /* --- CQ ring --- */
    r->cq_mmap_sz = (int)(p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe));
    r->cq_mmap = mmap(NULL, (size_t)r->cq_mmap_sz, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_CQ_RING);
    if (r->cq_mmap == MAP_FAILED) goto err_cq;

    r->cq_head     = (unsigned *)((char *)r->cq_mmap + p.cq_off.head);
    r->cq_tail     = (unsigned *)((char *)r->cq_mmap + p.cq_off.tail);
    r->cq_mask     = (unsigned *)((char *)r->cq_mmap + p.cq_off.ring_mask);
    r->cq_entries  = (unsigned *)((char *)r->cq_mmap + p.cq_off.ring_entries);
    r->cq_overflow = (unsigned *)((char *)r->cq_mmap + p.cq_off.overflow);
    r->cqes        = (struct io_uring_cqe *)((char *)r->cq_mmap + p.cq_off.cqes);

    return r;

err_cq:
    munmap(r->sqes_mmap, (size_t)r->sqes_mmap_sz);
err_sq:
    munmap(r->sq_mmap, (size_t)r->sq_mmap_sz);
err:
    close(r->ring_fd);
    free(r);
    return NULL;
}

void iouring_destroy(iouring_t *r)
{
    if (!r) return;
    munmap(r->cq_mmap,    (size_t)r->cq_mmap_sz);
    munmap(r->sqes_mmap,  (size_t)r->sqes_mmap_sz);
    munmap(r->sq_mmap,    (size_t)r->sq_mmap_sz);
    close(r->ring_fd);
    pthread_mutex_destroy(&r->sq_mtx);
    free(r);
}

struct io_uring_sqe* iouring_get_sqe(iouring_t *r)
{
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
    unsigned next = tail + 1;
    if (next - head > *r->sq_entries) return NULL;
    return &r->sqes[tail & *r->sq_mask];
}

void iouring_sqe_ready(iouring_t *r)
{
    unsigned tail  = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE);
    unsigned index = tail & *r->sq_mask;
    r->array[index] = index;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
}

int iouring_submit_and_wait(iouring_t *r, unsigned wait_nr)
{
    if (r->sqpoll) {
        /* SQPOLL: the kernel thread submits SQEs; userspace only waits for CQEs.
         * If the kernel thread is asleep (NEED_WAKEUP), add the wakeup flag. */
        unsigned flags = __atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE);
        unsigned enter_flags = IORING_ENTER_GETEVENTS;
        if (flags & IORING_SQ_NEED_WAKEUP)
            enter_flags |= IORING_ENTER_SQ_WAKEUP;
        return io_uring_enter(r->ring_fd, 0, wait_nr, enter_flags);
    }
    unsigned submit = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE)
                    - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    return io_uring_enter(r->ring_fd, submit, wait_nr, IORING_ENTER_GETEVENTS);
}

int iouring_submit(iouring_t *r)
{
    if (r->sqpoll) {
        /* SQPOLL: only wake the kernel thread on NEED_WAKEUP */
        unsigned flags = __atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE);
        if (flags & IORING_SQ_NEED_WAKEUP)
            return io_uring_enter(r->ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP);
        return 0;
    }
    unsigned submit = __atomic_load_n(r->sq_tail, __ATOMIC_ACQUIRE)
                    - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    if (submit == 0) return 0;
    return io_uring_enter(r->ring_fd, submit, 0, 0);
}

struct io_uring_cqe* iouring_get_cqe(iouring_t *r)
{
    unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
    if (head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE)) return NULL;
    return &r->cqes[head & *r->cq_mask];
}

void iouring_cqe_seen(iouring_t *r, struct io_uring_cqe *cqe)
{
    (void)cqe;
    __atomic_store_n(r->cq_head,
                     __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE) + 1,
                     __ATOMIC_RELEASE);
}

int iouring_register_buffers(iouring_t *r, struct iovec *iovecs, unsigned nr_iovecs)
{
    return (int)syscall(__NR_io_uring_register, r->ring_fd,
                        IORING_REGISTER_BUFFERS, iovecs, nr_iovecs);
}
