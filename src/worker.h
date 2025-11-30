#ifndef WORKER_H
#define WORKER_H

#include "config.h"     // server_config_t (usa cache_size_mb, num_workers, document_root, etc.)
#include "cache.h"      // file_cache_t (Feature 4: cache por worker)
#include "shared_mem.h" // Estruturas de memória partilhada (fila de conexões, stats)
#include "semaphores.h" // Semáforos para sincronização da fila

// ###################################################################################################################
// Worker Lifecycle API
// ###################################################################################################################

// Inicializa recursos específicos do worker (executado no processo filho após o fork do master).
// cfg -> Configuração carregada (usa cache_size_mb/num_workers para dimensionar a cache e define document_root).
void worker_init_resources(const server_config_t* cfg);

// Liberta/fecha recursos específicos do worker (ex.: cache). Pode ser chamado no fim do worker_main.
// Sem argumentos, atua sobre o estado interno do worker (g_cache, etc.).
void worker_shutdown_resources(void);

// Acesso à cache do worker (para outros módulos do worker, ex.: thread_pool/http handler).
// Retorna ponteiro opaco para a cache LRU thread-safe.
file_cache_t* worker_get_cache(void);

// Acesso ao document_root do worker (string terminada em '\0' configurada em worker_init_resources).
// Retorna ponteiro constante para uso em construção do caminho absoluto dos ficheiros a servir.
const char* worker_get_document_root(void);

// ###################################################################################################################
// Worker Main Loop
// ###################################################################################################################

// Função principal do worker.
// shm        -> Ponteiro para a memória partilhada (fila de conexões, stats).
// sems       -> Conjunto de semáforos para proteger a fila (empty/filled/mutex).
// worker_id  -> Identificador lógico do worker (para logs/diagnóstico).
// channel_fd -> Socket UNIX por onde o master envia o descritor real do cliente (SCM_RIGHTS).
void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd);

#endif /* WORKER_H */
