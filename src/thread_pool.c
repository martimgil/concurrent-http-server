#include "thread_pool.h"
#include <stdlib.h>

// É o loop principal para um worker numa thread pool
// Espera pelo trabalho para estar disponviel e verifica se a pool esta a ser deslifada
// Processa as tarefas da queue
// Se o shutdown flag for ativado, sai do loop e termina a thread e o loop termina
// Garante que todas as threads terminem ao desligar a pool

// worker_thread is now defined in master.c

thread_pool_t* create_thread_pool(int num_threads) {
    thread_pool_t* pool = malloc(sizeof(thread_pool_t));
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    pool->num_threads = num_threads;
    pool->shutdown = 0;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool->threads[i], NULL, worker_thread, pool);
    }

    return pool;
}