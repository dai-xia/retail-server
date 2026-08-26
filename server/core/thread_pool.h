#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct task_s {
    void (*func)(void *arg);
    void *arg;
    void (*cleanup)(void *arg);
    struct task_s *next;
} task_t;

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_cond_t full_cond;
    pthread_t *threads;
    task_t *task_head;
    task_t *task_tail;
    int thread_count;
    int task_count;
    int max_task_count;
    int shutdown;
} thread_pool_t;

thread_pool_t* thread_pool_create(int thread_count, int max_task_count);
void thread_pool_destroy(thread_pool_t *pool);
int thread_pool_add_task(thread_pool_t *pool, void (*func)(void *arg), void *arg, void (*cleanup)(void *arg));
int thread_pool_get_task_count(thread_pool_t *pool);
int thread_pool_get_thread_count(thread_pool_t *pool);
int thread_pool_get_max_task_count(thread_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif