// Define as estrutura de dados e os prototipos das funções para gerir a memória compartinhada

#ifndef SHARED_MEM_H
#define SHARED_MEM_H
#define MAX_QUEUE_SIZE 100
typedef struct {
    long total_requests;
    long bytes_transferred;
    long status_200;
    long status_404;
    long status_500;
    int active_connections;
} server_stats_t;
typedef struct {
    int sockets[MAX_QUEUE_SIZE];
    int front;
    int rear;
    int count;
} connection_queue_t;
typedef struct {
    connection_queue_t queue;
    server_stats_t stats;
} shared_data_t; // Estrutura de dados que combina a queue e as estatisticas numa região single shared
shared_data_t* create_shared_memory(); // Aloca e inicializa o segnemento de memoria partilhado
void destroy_shared_memory(shared_data_t* data); //Limpa e lança a memória partilhada

#endif