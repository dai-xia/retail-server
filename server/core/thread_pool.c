#include "thread_pool.h"
#include <errno.h>

static void* worker_thread(void *arg)
{
    thread_pool_t *pool = (thread_pool_t*)arg;
    task_t *task;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    while (1) {
        pthread_mutex_lock(&pool->mutex);

        while (pool->task_head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }

        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            pthread_exit(NULL);
        }

        task = pool->task_head;
        pool->task_head = task->next;
        if (pool->task_head == NULL) pool->task_tail = NULL;
        pool->task_count--;

        if (pool->max_task_count > 0)
            pthread_cond_signal(&pool->full_cond);

        pthread_mutex_unlock(&pool->mutex);

        task->func(task->arg);
        /* cleanup is only invoked when the pool is shutting down and tasks
         * are dropped. It must NOT be called after func runs normally,
         * otherwise it would double free. */
        free(task);
    }
    return NULL;
}

thread_pool_t* thread_pool_create(int thread_count, int max_task_count)
{
    if (thread_count <= 0 || thread_count > 100) return NULL;
    if (max_task_count < 0) return NULL;

    thread_pool_t *pool = malloc(sizeof(thread_pool_t));
    if (!pool) return NULL;
    memset(pool, 0, sizeof(thread_pool_t));

    pool->thread_count = thread_count;
    pool->max_task_count = max_task_count;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pthread_cond_init(&pool->full_cond, NULL);

    pool->threads = malloc(sizeof(pthread_t) * thread_count);
    if (!pool->threads) {
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->cond);
        pthread_cond_destroy(&pool->full_cond);
        free(pool);
        return NULL;
    }

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->cond);
            for (int j = 0; j < i; j++) pthread_join(pool->threads[j], NULL);
            free(pool->threads);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->cond);
            pthread_cond_destroy(&pool->full_cond);
            free(pool);
            return NULL;
        }
    }

    printf("thread pool created: %d threads, queue limit %d\n", thread_count, max_task_count);
    return pool;
}

void thread_pool_destroy(thread_pool_t *pool)
{
    if (!pool) return;

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_cond_broadcast(&pool->full_cond);
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < pool->thread_count; i++) pthread_join(pool->threads[i], NULL);

    pthread_mutex_lock(&pool->mutex);
    task_t *task = pool->task_head;
    while (task) {
        task_t *next = task->next;
        if (task->cleanup) task->cleanup(task->arg);
        free(task);
        task = next;
    }
    pthread_mutex_unlock(&pool->mutex);

    free(pool->threads);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pthread_cond_destroy(&pool->full_cond);
    free(pool);
    printf("thread pool destroyed\n");
}

int thread_pool_add_task(thread_pool_t *pool, void (*func)(void *arg), void *arg, void (*cleanup)(void *arg))
{
    if (!pool || !func) return -1;

    task_t *task = malloc(sizeof(task_t));
    if (!task) return -1;
    task->func = func;
    task->arg = arg;
    task->cleanup = cleanup;
    task->next = NULL;

    pthread_mutex_lock(&pool->mutex);

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        if (cleanup) cleanup(arg);
        free(task);
        return -1;
    }

    if (pool->max_task_count > 0) {
        while (pool->task_count >= pool->max_task_count && !pool->shutdown) {
            pthread_cond_wait(&pool->full_cond, &pool->mutex);
        }
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            if (cleanup) cleanup(arg);
            free(task);
            return -1;
        }
    }

    if (!pool->task_tail) {
        pool->task_head = task;
    } else {
        pool->task_tail->next = task;
    }
    pool->task_tail = task;
    pool->task_count++;

    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}

int thread_pool_get_task_count(thread_pool_t *pool)
{
    if (!pool) return -1;
    pthread_mutex_lock(&pool->mutex);
    int count = pool->task_count;
    pthread_mutex_unlock(&pool->mutex);
    return count;
}

int thread_pool_get_thread_count(thread_pool_t *pool)
{
    return pool ? pool->thread_count : -1;
}

int thread_pool_get_max_task_count(thread_pool_t *pool)
{
    return pool ? pool->max_task_count : -1;
}