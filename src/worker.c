#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <string.h>

#include "worker.h"
#include "shared_mem.h"
#include "semaphores.h"
#include "thread_pool.h"
#include "config.h"
#include "cache.h"     // ADICIONADO: Interface da cache (Feature 4)
#include "logger.h"    // ADICIONADO: Thread-safe logging (Feature 5)

// ###################################################################################################################
// Sending file descriptors over UNIX sockets
// https://gist.github.com/domfarolino/4293951bd95082125f2b9931cab1de40
// ###################################################################################################################

// Function to receive a file descriptor over a Unix domain socket
// socket -> Socket file descriptor
// Retorna o descritor de ficheiro recebido, ou -1 em caso de erro
static int recv_fd(int socket){
    // msghdr -> Message header structure
    // msg -> Message to be received
    // {0} -> Initialize all fields to zero
    struct msghdr msg = {0}; 
   
    // buf -> Buffer to hold a single byte (required for recvmsg)
    char buf[1];

    // iovec -> I/O vector structure
    // io -> I/O vector for the message
    // .iov_base -> Pointer to the buffer
    // .iov_len -> Length of the buffer
    struct iovec io = { .iov_base = buf, .iov_len = 1 };

    // Union to hold control message
    union {

        // buf -> Buffer for control message ancillary data
        // CMSG_SPACE -> Macro to calculate space needed for control message
        // sizeof(int) -> Size of the file descriptor
        char buf[CMSG_SPACE(sizeof(int))];

        // align -> cmsghdr structure for alignment
        // cmsghdr -> Control message header structure
        struct cmsghdr align;
    } u; // u -> Union to hold control message


    // Set up the message header
    // msg.msg_iov -> Pointer to the I/O vector
    // &io -> Address of the I/O vector
    msg.msg_iov = &io;

    // msg.msg_iovlen -> Number of I/O vectors
    // 1 -> Number of I/O vectors
    msg.msg_iovlen = 1;

    // msg.msg_control -> Pointer to control message buffer
    // u.buf -> Address of the control message buffer
    msg.msg_control = u.buf;

    // msg.msg_controllen -> Length of control message buffer
    // sizeof(u.buf) -> Size of the control message buffer
    msg.msg_controllen = sizeof(u.buf);


    // recvmsg -> Receive message on socket
    // socket -> Socket file descriptor
    // &msg -> Address of the message header
    // 0 -> No special flags
    // < 0 -> Error occurred
    // == 0 -> Success (but we expect ancillary data)
    if (recvmsg(socket, &msg, 0) < 0){
        perror("Failed to receive fd");
        return -1;
    }

    // cmsghdr -> Control message header
    // CMSG_FIRSTHDR -> Macro to get the first control message header
    // &msg -> Address of the message header
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    // Extract the file descriptor from the control message
    // cmsg -> Control message header
    // CMSG_LEN -> Macro to calculate length of control message
    // sizeof(int) -> Size of an integer (file descriptor)
    if (cmsg && (cmsg->cmsg_len == CMSG_LEN(sizeof(int)))){

        // Validate the control message
        // cmsg->cmsg_level -> Level of the control message
        // SOL_SOCKET -> Socket level 
        // cmsg->cmsg_type -> Type of the control message
        // SCM_RIGHTS -> Passing file descriptor rights
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            return -1;
        }

        // Return the received file descriptor
        // CMSG_DATA -> Macro to get pointer to control message data
        // (int*) -> Cast to integer pointer
        // cmsg -> Control message header
        return *((int *)CMSG_DATA(cmsg));
    }
    
    // Nenhuma ancillary data válida encontrada
    return -1;
}


// ###################################################################################################################
// STATE (estado global do worker)
// ###################################################################################################################

// Global variable to control the worker loop
static volatile int worker_running = 1;

// Cache por worker (Feature 4: Thread-Safe File Cache)
// Cada processo worker tem a sua própria instância de cache (não partilhada entre processos).
static file_cache_t* g_cache = NULL;

// Document root por worker (cada processo sabe a sua raíz de documentos)
// Mantemos uma cópia local a partir da configuração no arranque do worker.
static char g_docroot[256];   // ADICIONADO: document root local ao worker


// ###################################################################################################################
// SIGNAL HANDLER
// ###################################################################################################################

// Function to start the worker process
// Handler de sinais para terminar o worker de forma graciosa
void worker_signal_handler(int signum){
    (void)signum;
    worker_running = 0;
}


// ###################################################################################################################
// FEATURE 4: Cache — inicialização, acesso e destruição de recursos
// ###################################################################################################################

// Inicializa recursos do worker dependentes de configuração (ex.: cache + document_root)
// Arguments:
//  cfg -> Ponteiro para configuração carregada (usa cache_size_mb e num_workers para dimensionar a cache)
void worker_init_resources(const server_config_t* cfg) {

    // Guardar o DOCUMENT_ROOT numa área local do worker (string terminada por '\0')
    // cfg->document_root -> Diretório raíz para servir ficheiros
    // strncpy -> Copia com limite para evitar overflow
    strncpy(g_docroot, cfg->document_root, sizeof(g_docroot) - 1);

    // Garantir terminação NUL para segurança
    g_docroot[sizeof(g_docroot) - 1] = '\0';

    // ----------------------------------------------------------------------
    // ADICIONADO (Feature 5):
    // Inicializar o logger thread-safe/process-safe
    // (cada worker reabre o mesmo ficheiro de log com semáforo global)
    // ----------------------------------------------------------------------
    logger_init(cfg->log_file);

    // Capacidade total desejada em bytes (configuração dá em megabytes)
    size_t cap = (size_t)cfg->cache_size_mb * 1024ULL * 1024ULL;

    // Opcional: dividir a capacidade total pela quantidade de workers,
    // para evitar que cada worker tente usar a totalidade da memória.
    if (cfg->num_workers > 0) {
        cap /= (size_t)cfg->num_workers;
        // Impor um mínimo razoável para evitar caches demasiado pequenas.
        if (cap < (1u << 20)) cap = (1u << 20); // mínimo 1 MiB
    }

    // Criar a cache LRU thread-safe
    g_cache = cache_create(cap);

    // Verificação de falha crítica
    if (!g_cache) {
        fprintf(stderr, "WORKER: Falha ao criar cache (cap=%zu bytes).\n", cap);
        // Não conseguimos servir conteúdo eficientemente — abortar worker
        exit(1);
    }

    // Log informativo para debugging
    fprintf(stderr, "Worker: Cache inicializada com %zu bytes. DOCROOT=%s\n", cap, g_docroot);
}

// Getter para a cache do worker
// Retorna o ponteiro opaco da cache para consumo por outros módulos (ex.: thread_pool/http handler)
file_cache_t* worker_get_cache(void) {
    return g_cache;
}

// Getter para o document root do worker
// Retorna ponteiro constante para a raíz de documentos (string interna; não modificar)
const char* worker_get_document_root(void) {   // ADICIONADO: acesso ao document_root
    return g_docroot;
}

// Destrói e limpa os recursos do worker (cache, etc.)
void worker_shutdown_resources(void) {
    if (g_cache) {
        cache_destroy(g_cache);
        g_cache = NULL;
    }

    // ----------------------------------------------------------------------
    // ADICIONADO (Feature 5):
    // Fechar logger neste processo worker
    // ----------------------------------------------------------------------
    logger_close();
}


// ###################################################################################################################
// FEATURE 1: Connection Queue Consumer
// ###################################################################################################################

// Worker main function
// Wait for connections in the shared memory queue 
// Block queue
// Dequeue connection
// Unblock queue
// Mark slot as empty
// Process connection

void worker_main(shared_data_t* shm, semaphores_t* sems, int worker_id, int channel_fd){

    // Register signal handler proprietary to this worker

    // signal -> Start the worker process
    // SIGTERM and SIGINT will stop the worker process
    signal(SIGTERM, worker_signal_handler);
    signal(SIGINT, worker_signal_handler);

    // Create a thread pool with 10 threads
    // Nota: o thread pool irá, tipicamente, chamar a lógica que serve pedidos (HTTP) e,
    //       nessa lógica, deverá usar a cache através de worker_get_cache() e o DOCROOT via worker_get_document_root().
    thread_pool_t* pool = create_thread_pool(10, shm, sems);

    printf("Worker %d: Starting main loop.\n", worker_id);

    while(worker_running){

        // Wait for an available connection
        // Tratar EINTR para que o Ctrl+C/SIGTERM acorde o worker
        while (worker_running) {
            if (sem_wait(sems->filled_slots) == 0) break;
            if (errno == EINTR) {
                if (!worker_running) break;  // sair no shutdown
                continue; // sinal transitório — repetir wait
            }
            perror("Worker sem_wait(filled_slots) error");
            // erro não-EINTR: tenta continuar o loop principal
            break;
        }
        if (!worker_running) break;

        // Exclusive access to queue
        while (worker_running) {
            if (sem_wait(sems->queue_mutex) == 0) break;
            if (errno == EINTR) {
                if (!worker_running) break;
                continue;
            }
            perror("Worker sem_wait(queue_mutex) error");
            // Falhou adquirir mutex: devolver slot preenchido para não perder sinal
            sem_post(sems->filled_slots);
            // e seguir para próximo ciclo
            goto next_iteration;
        }
        if (!worker_running) {
            // se saímos por shutdown, não deixar o mutex agarrado
            // (aqui só se entrou no while se tivesse conseguido lock)
            // portanto nada a fazer
            break;
        }

        // Dequeue client socket 
        // shm -> Shared memory structure
        // queue.sockets -> Array of client sockets in the queue
        // queue.front -> Index of the front of the queue

        // Ignore the client socket
        // shm -> Shared memory structure
        // queue.front -> Index of the front of the queue
        int ignore_fd = shm->queue.sockets[shm->queue.front];
        (void)ignore_fd; // We don't use it here since we receive via UNIX socket
        
        // Update queue front and count
        // queue.size -> Maximum size of the queue

        shm->queue.front = (shm->queue.front + 1) % MAX_QUEUE_SIZE;
        
        // Decrease the count of items in the queue
        shm->queue.count--;


        // Free mutex and signal that a slot is now free
        // sems -> queue_mutex --> Release mutex semaphore for queue
        // sems -> empty_slots --> Signal that an empty slot is available
        sem_post(sems->queue_mutex);
        sem_post(sems->empty_slots);

        // Receive the client file descriptor from the master via UNIX socket
        // channel_fd -> Channel file descriptor
        // recv_fd -> Function to receive file descriptor
        // client_fd -> Client socket file descriptor
        {
            int client_fd = recv_fd(channel_fd);

            // Error handling
            if (client_fd < 0) {
                if (!worker_running) break; // shutdown: sair
                fprintf(stderr, "Worker %d: Falha ao receber descritor real.\n", worker_id);
                goto next_iteration;
            }

            // Process the client request
            printf("Worker %d: Processing client socket %d\n", worker_id, client_fd);
            
            // Submit the client request to the thread pool
            // thread_pool_submit -> entrega o descritor para processamento assíncrono
            // O handler associado ao pool deve:
            //   1) Interpretar o pedido HTTP
            //   2) Consultar/usar a cache com worker_get_cache()
            //   3) Obter o document root com worker_get_document_root()
            //   4) Escrever a resposta no socket
            //   5) Fechar o socket no fim do processamento
            thread_pool_submit(pool, client_fd);
        }

    next_iteration:
        ; // no-op label target
    }

    // Cleanup: destroy the thread pool before exiting
    printf("Worker %d: Shutting down thread pool.\n", worker_id);
    destroy_thread_pool(pool);

    // Destruição dos recursos específicos do worker (ex.: cache)
    worker_shutdown_resources();
}
