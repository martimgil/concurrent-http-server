#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <pthread.h>

typedef struct {
    pthread_t* threads;
    int num_threads;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int shutdown;
} thread_pool_t;

void* worker_thread(void* arg);

thread_pool_t* create_thread_pool(int num_threads);
void destroy_thread_pool(thread_pool_t* pool);
#endif