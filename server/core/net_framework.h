#ifndef __NET_FRAMEWORK_H
#define __NET_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <pthread.h>
#include <time.h>

/* Export C symbols so C++ callers are not affected by name mangling */
#ifdef __cplusplus
extern "C" {
#endif

#define NET_MAX_EVENTS      1024
#define NET_BUF_SIZE        65536
#define NET_HEAD_LEN        4           /* frame header length: holds body length */
#define NET_READ_BUF_SIZE   65536
#define NET_SUB_REACTOR_NUM 4
#define NET_THREAD_POOL_SIZE   8
#define NET_THREAD_POOL_MAX_TASK 256

typedef enum {
    NET_BACKEND_EPOLL = 0,
    NET_BACKEND_IOURING = 1,
} net_backend_t;

#include "thread_pool.h"

/* Forward declarations to break circular struct references */
struct connection_s;
typedef struct connection_s connection_t;
struct net_framework_s;
typedef struct net_framework_s net_framework_t;

/* Queued send tasks per connection. Queue avoids concurrent write races. */
typedef struct write_task_s {
    char *data;
    int len;
    int offset;
    struct write_task_s *next;
} write_task_t;

/* Backend vtable: abstracts epoll / io_uring behind a uniform interface */
typedef struct {
    void (*destroy)(net_framework_t *nf);
    int  (*start)(net_framework_t *nf);
    void (*stop)(net_framework_t *nf);
    void (*submit_write)(connection_t *c);
    void (*close_conn)(connection_t *c);
} net_framework_ops_t;

struct net_framework_s {
    int port;
    int running;
    int backend;
    void *user_data;

    /* Network event callbacks implemented by the upper business layer */
    void (*on_accept)(connection_t *conn);
    void (*on_recv)(connection_t *conn, const char *data, int len);
    void (*on_close)(connection_t *conn);
    void (*on_error)(connection_t *conn, int err);

    thread_pool_t *thread_pool;

    const net_framework_ops_t *ops;
    void *impl;
};

struct connection_s {
    int fd;
    int closed;
    int ref;                            /* refcount: prevents premature free under multithreading */
    int backend;

    char client_ip[16];
    int client_port;
    void *user_data;
    net_framework_t *nf;

    /* Read buffer for incoming client data */
    char rbuf[NET_READ_BUF_SIZE];
    char *rbuf_ptr;
    int rlen;
    uint32_t expect_len;                /* expected full frame length (for unpacking) */

    /* Thread-safe write task queue */
    pthread_mutex_t write_lock;
    write_task_t *write_head;
    write_task_t *write_tail;
    int write_count;

    time_t last_active;                /* for heartbeat / idle timeout */

    void *backend_data;
};

extern int nf_epoll_init(net_framework_t *nf);
extern int nf_iouring_init(net_framework_t *nf);

/**
 * @brief Create the network framework, defaulting to the epoll backend
 * @param port listen port
 * @return framework pointer on success, NULL on failure
 */
net_framework_t* net_framework_create(int port);

/**
 * @brief Create the network framework with a chosen IO backend (epoll / io_uring)
 * @param port listen port
 * @param backend backend type
 * @return framework pointer on success, NULL on failure
 */
net_framework_t* net_framework_create_with_backend(int port, net_backend_t backend);

/**
 * @brief Destroy the framework and release all resources
 */
void net_framework_destroy(net_framework_t *nf);

/**
 * @brief Start the network service and begin handling connections
 * @return 0 on success, -1 on failure
 */
int net_framework_start(net_framework_t *nf);

/**
 * @brief Stop the network service
 */
void net_framework_stop(net_framework_t *nf);

/**
 * @brief Register network event callbacks (accept, recv, close, error)
 */
void net_framework_set_callbacks(net_framework_t *nf,
                                 void (*on_accept)(connection_t *conn),
                                 void (*on_recv)(connection_t *conn, const char *data, int len),
                                 void (*on_close)(connection_t *conn),
                                 void (*on_error)(connection_t *conn, int err));

/**
 * @brief Send a JSON packet
 * @return 0 on success, -1 on failure
 */
int net_send_packet(connection_t *conn, const char *json_data);

/**
 * @brief Send raw binary data
 * @return 0 on success, -1 on failure
 */
int net_send_binary(connection_t *conn, const char *data, int len);

/**
 * @brief Actively close a client connection
 */
void net_close_connection(connection_t *conn);

/**
 * @brief Set a file descriptor to non-blocking mode
 * @return 0 on success, -1 on failure
 */
int net_set_nonblocking(int fd);

/**
 * @brief Increment the connection refcount (protect from premature release)
 */
void net_conn_ref(connection_t *conn);

/**
 * @brief Decrement the connection refcount; release the connection at zero
 */
void net_conn_unref(connection_t *conn);

/**
 * @brief Probe whether the running Linux supports io_uring
 * @return 1 if supported, 0 otherwise
 */
int net_probe_io_uring_support(void);

/**
 * @brief Core frame parser: extracts complete frames from the read buffer,
 *        handling TCP packet framing (sticky/half-packet issues).
 *
 * TCP is a byte stream with no boundaries, so framing relies on a custom
 * protocol (typically the first 4 bytes hold the body length, NET_HEAD_LEN).
 * Each complete frame triggers on_recv; leftover partial data stays in the
 * buffer to be combined with the next read.
 *
 * @param c          client connection
 * @param nf         owning framework
 * @param bytes_read bytes actually read this round
 * @return 0 on success, -1 if the connection should be closed
 */
int net_parse_frames(connection_t *c, net_framework_t *nf, int bytes_read);

/**
 * @brief Enqueue data for sending on a connection and trigger the backend send
 */
void net_queue_write(connection_t *c, const char *data, int len);

#ifdef __cplusplus
}
#endif

#endif // __NET_FRAMEWORK_H
