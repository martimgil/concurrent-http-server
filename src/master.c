// ###################################################################################################################
// master.c — Processo mestre do servidor HTTP concorrente
//
// Responsabilidades:
//  - Ler configuração (server.conf)
//  - Criar socket TCP de escuta (bind/listen)
//  - Criar memória partilhada e semáforos (fila de conexões)
//  - Criar N processos worker e um canal UNIX (socketpair) por worker
//  - Aceitar conexões e distribuí-las (round-robin) enviando o FD real via SCM_RIGHTS
//  - Shutdown gracioso em SIGINT/SIGTERM
// ###################################################################################################################

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "config.h"       // server_config_t, load_config()
#include "shared_mem.h"   // create_shared_memory(), destroy_shared_memory()
#include "semaphores.h"   // init_semaphores(), destroy_semaphores()
#include "worker.h"       // worker_init_resources(), worker_main(), worker_shutdown_resources()
#include "logger.h"       // logger_init/logger_close (Feature 5)

// ###################################################################################################################
// Estado global e handlers de sinal do master
// ###################################################################################################################

static volatile int master_running = 1;

// Handler de sinal para terminar o master de forma graciosa
static void master_signal_handler(int signum) {
    (void)signum;
    master_running = 0;
}

// ###################################################################################################################
// Utilitários: socket TCP (listen) e canal UNIX (socketpair)
// ###################################################################################################################

// Criação de socket servidor TCP (IPv4), bind e listen
// Arguments:
//  port -> Porta TCP
// Return:
//  descritor de socket em sucesso; -1 em erro
static int create_listen_socket(int port) {

    // socket(AF_INET, SOCK_STREAM, 0) -> TCP IPv4
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    // SO_REUSEADDR -> reutilização rápida da porta após restart
    int yes = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(s);
        return -1;
    }

    // bind a 0.0.0.0:port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }

    // listen com backlog razoável
    if (listen(s, 1024) < 0) {
        perror("listen");
        close(s);
        return -1;
    }

    return s;
}

// Criação de canal mestre<->worker por socketpair AF_UNIX (DGRAM)
// Arguments:
//  sv[2] -> retorna os dois descritores (sv[0] fica no master, sv[1] vai para o worker)
// Return:
//  0 em sucesso; -1 em erro
static int create_worker_channel(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        perror("socketpair");
        return -1;
    }
    return 0;
}

// Enviar descritor de cliente a um worker via SCM_RIGHTS
// Arguments:
//  socket -> extremidade do master (sv[0])
//  fd     -> descritor do cliente aceitado no listen
// Return:
//  0 em sucesso; -1 em erro
static int send_fd(int socket, int fd) {
    struct msghdr msg = {0};
    char buf[1] = {'F'}; // payload "dummy" obrigatório
    struct iovec io = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;

    memset(&u, 0, sizeof(u));

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));

    *((int*) CMSG_DATA(cmsg)) = fd;

    if (sendmsg(socket, &msg, 0) < 0) {
        perror("sendmsg(SCM_RIGHTS)");
        return -1;
    }
    return 0;
}

// ###################################################################################################################
// Inicialização de Memória Partilhada e Semáforos (usa a API real do teu projeto)
// ###################################################################################################################

// Shared memory real: create_shared_memory(queue_size)
static shared_data_t* init_shared_memory(int queue_size) {

    // Espera-se que o módulo shared_mem trate de shm_open/ftruncate/mmap/init da fila (front=0,count=0)
    shared_data_t* shm = create_shared_memory(queue_size);
    if (!shm) {
        fprintf(stderr, "MASTER: shared_mem_create() failed.\n");
        return NULL;
    }
    return shm;
}

// Semáforos reais: init_semaphores(), destroy_semaphores()
static semaphores_t* init_semaphore_system(int queue_size) {

    semaphores_t* sems = (semaphores_t*)malloc(sizeof(semaphores_t));
    if (!sems) {
        fprintf(stderr, "MASTER: malloc failed for semaphores\n");
        return NULL;
    }

    if (init_semaphores(sems, queue_size) != 0) {
        fprintf(stderr, "MASTER: init_semaphores() failed\n");
        free(sems);
        return NULL;
    }

    return sems;
}

// ###################################################################################################################
// Enqueue na fila partilhada (sinalização de capacidade/contagem)
// O worker ignora o valor armazenado (pois recebe o FD real via SCM_RIGHTS).
// ###################################################################################################################

// Retorna 0 em sucesso; -1 em erro
static int enqueue_connection(shared_data_t* shm, semaphores_t* sems, int placeholder_fd) {

    // Esperar por um slot livre
    if (sem_wait(sems->empty_slots) != 0) {
        perror("sem_wait(empty_slots)");
        return -1;
    }

    // Zona crítica de acesso à fila
    if (sem_wait(sems->queue_mutex) != 0) {
        perror("sem_wait(queue_mutex)");
        // Repor o slot livre em caso de falha
        sem_post(sems->empty_slots);
        return -1;
    }

    // Inserir na posição rear (front + count) % MAX_QUEUE_SIZE
    int pos = (shm->queue.front + shm->queue.count) % MAX_QUEUE_SIZE;
    shm->queue.sockets[pos] = placeholder_fd; // marcador (não será usado pelo worker)
    shm->queue.count++;

    // Libertar zona crítica
    sem_post(sems->queue_mutex);

    // Sinalizar item disponível
    sem_post(sems->filled_slots);

    return 0;
}

// ###################################################################################################################
// Função principal do processo master
// ###################################################################################################################

int main(int argc, char* argv[]) {

    // ---------------------------------------------------------------------------------------------------------------
    // 1) Carregar configuração
    // ---------------------------------------------------------------------------------------------------------------
    const char* conf_path = (argc >= 2) ? argv[1] : "server.conf";

    server_config_t config;
    memset(&config, 0, sizeof(config));

    // Defaults razoáveis (caso falhe leitura do ficheiro)
    config.port               = 8080;
    strncpy(config.document_root, "www", sizeof(config.document_root) - 1);
    config.num_workers        = 2;
    config.threads_per_worker = 10;
    config.max_queue_size     = MAX_QUEUE_SIZE;
    strncpy(config.log_file, "logs/access.log", sizeof(config.log_file) - 1);
    config.cache_size_mb      = 64;
    config.timeout_seconds    = 30;

    if (load_config(conf_path, &config) != 0) {
        fprintf(stderr, "MASTER: Using defaults (failed to load %s)\n", conf_path);
    } else {
        fprintf(stderr, "MASTER: Config loaded from %s\n", conf_path);
    }

    // ADICIONADO: inicializar o logger global (Feature 5)
    logger_init(config.log_file);

    // ---------------------------------------------------------------------------------------------------------------
    // 2) Handlers de sinal (CTRL+C, kill)
    // ---------------------------------------------------------------------------------------------------------------
    signal(SIGINT,  master_signal_handler);
    signal(SIGTERM, master_signal_handler);

    // ---------------------------------------------------------------------------------------------------------------
    // 3) Memória partilhada e semáforos
    // ---------------------------------------------------------------------------------------------------------------
    shared_data_t* shm = init_shared_memory(config.max_queue_size);
    if (!shm) {
        return 1;
    }

    semaphores_t* sems = init_semaphore_system(config.max_queue_size);
    if (!sems) {
        shm_destroy(shm);
        return 1;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 4) Listen socket
    // ---------------------------------------------------------------------------------------------------------------
    int listen_fd = create_listen_socket(config.port);
    if (listen_fd < 0) {
        destroy_semaphores(sems);
        free(sems);
        shm_destroy(shm);
        return 1;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 5) Criar N workers (fork) e um canal UNIX por worker
    // ---------------------------------------------------------------------------------------------------------------
    int num_workers = (config.num_workers > 0) ? config.num_workers : 1;

    pid_t* pids       = (pid_t*)calloc((size_t)num_workers, sizeof(pid_t));
    int*   parent_end = (int*)  calloc((size_t)num_workers, sizeof(int));
    if (!pids || !parent_end) {
        fprintf(stderr, "MASTER: alloc failed\n");
        close(listen_fd);
        destroy_semaphores(sems);
        free(sems);
        shm_destroy(shm);
        free(pids); free(parent_end);
        return 1;
    }

    for (int i = 0; i < num_workers; ++i) {
        int sv[2];
        if (create_worker_channel(sv) != 0) {
            fprintf(stderr, "MASTER: failed to create channel for worker %d\n", i);
            // limpeza parcial
            for (int k = 0; k < i; ++k) close(parent_end[k]);
            close(listen_fd);
            destroy_semaphores(sems);
            free(sems);
            shm_destroy(shm);
            free(pids); free(parent_end);
            return 1;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(sv[0]); close(sv[1]);
            for (int k = 0; k < i; ++k) close(parent_end[k]);
            close(listen_fd);
            destroy_semaphores(sems);
            free(sems);
            shm_destroy(shm);
            free(pids); free(parent_end);
            return 1;
        }

        if (pid == 0) {
            // -------------------------------------------------------------------------------------------------------
            // Processo FILHO (WORKER)
            // -------------------------------------------------------------------------------------------------------
            // Fechar extremidade do master neste processo
            close(sv[0]);

            // O worker não precisa do listen_fd
            close(listen_fd);

            // Inicializar recursos do worker (ex.: cache por worker)
            worker_init_resources(&config);

            // Entrar no loop principal do worker
            worker_main(shm, sems, i, sv[1]);

            // Nota: worker_main já chama worker_shutdown_resources() no final
            _exit(0);
        }

        // -----------------------------------------------------------------------------------------------------------
        // Processo PAI (MASTER)
        // -----------------------------------------------------------------------------------------------------------
        pids[i] = pid;
        parent_end[i] = sv[0]; // master fica com esta extremidade
        close(sv[1]);          // fecha a extremidade do worker no master
    }

    fprintf(stderr, "MASTER: listening on port %d with %d workers.\n", config.port, num_workers);

    // ---------------------------------------------------------------------------------------------------------------
    // 6) Loop principal: accept + distribuição round-robin
    // ---------------------------------------------------------------------------------------------------------------
    int rr = 0; // índice de round-robin
    while (master_running) {

        // accept() bloqueante
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client_fd = accept(listen_fd, (struct sockaddr*)&cli, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR && !master_running) break; // interrompido por sinal
            perror("accept");
            continue;
        }

        // (Opcional) Tornar não-bloqueante:
        // int flags = fcntl(client_fd, F_GETFL, 0);
        // fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        // Enfileirar marcação na fila partilhada (capacidade/contagem)
        if (enqueue_connection(shm, sems, client_fd) != 0) {
            // Se a fila estiver cheia ou erro de semáforos, recusar ligação
            close(client_fd);
            continue;
        }

        // Escolher worker por round-robin
        int w = rr;
        rr = (rr + 1) % num_workers;

        // Enviar o FD real ao worker “w” via SCM_RIGHTS
        if (send_fd(parent_end[w], client_fd) != 0) {
            // Se falhar envio, fechar localmente
            close(client_fd);
            // (Opcional) registar estatística/erro
        }

        // O master já não precisa do FD depois de o enviar
        close(client_fd);
    }

    // ---------------------------------------------------------------------------------------------------------------
    // 7) Shutdown gracioso
    // ---------------------------------------------------------------------------------------------------------------
    fprintf(stderr, "MASTER: shutting down...\n");

    // Fechar listen
    close(listen_fd);

    // Fechar canais e sinalizar workers
    for (int i = 0; i < num_workers; ++i) {
        close(parent_end[i]);
        if (pids[i] > 0) kill(pids[i], SIGTERM);
    }

    // Esperar pela terminação dos workers
    for (int i = 0; i < num_workers; ++i) {
        if (pids[i] > 0) {
            int status = 0;
            waitpid(pids[i], &status, 0);
        }
    }

    // Libertar recursos do master
    destroy_semaphores(sems);
    free(sems);
    shm_destroy(shm);
    free(pids);
    free(parent_end);

    // ADICIONADO: fechar o logger global
    logger_close();

    fprintf(stderr, "MASTER: bye.\n");
    return 0;
}
