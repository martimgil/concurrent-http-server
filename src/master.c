#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include "semaphores.h"
#include "shared_mem.h"
#include <sys/shm.h>
#include <errno.h>
#include <string.h>

// Implementa a logica principal de aceitar conexoes do cliente.
// É aqui a parte central da arquitetira multi-thread do servidor.

#define PORT 8080
#define MAX_WORKERS 4
#define MAX_QUEUE_SIZE 100

typedef struct {
    shared_data_t* data;
    semaphores_t* sems;
} thread_args_t;

volatile sig_atomic_t keep_running = 1;

void signal_handler(int signum) {
    keep_running = 0;
}

int create_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        return -1;
    }

    if (listen(sockfd, 128) < 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static void reject_with_503(int client_fd) {
    const char* response = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
    ssize_t ignored = write(client_fd, response, strlen(response));
    (void)ignored;
    close(client_fd);
}

void enqueue_connection(shared_data_t* data, semaphores_t* sems, int client_fd) {
    if (sem_trywait(sems->empty_slots) != 0) {
        if (errno == EAGAIN) { // When semaphore couldn't decrement because the queue is full
            reject_with_503(client_fd);
            return;
        } else {
            reject_with_503(client_fd);
            return;
        }
    }

    sem_wait(sems->empty_slots);
    sem_wait(sems->queue_mutex);

    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    sem_post(sems->queue_mutex);
    sem_post(sems->filled_slots);
}

int dequeue_connection(shared_data_t* data, semaphores_t* sems) {
    // Semaphores wait
    sem_wait(sems->filled_slots); // Wait until there is a connection in the queue
    sem_wait(sems->queue_mutex); // Wait until the queue mutex is free

    int client_fd = data->queue.sockets[data->queue.front]; // Get the front connection

    data->queue.front = (data->queue.front + 1) % MAX_QUEUE_SIZE; // Move front pointer
    data->queue.count--; // Decrement queue size

    sem_post(sems->queue_mutex); // Release queue mutex
    sem_post(sems->empty_slots); // Release empty slots semaphore

    return client_fd;
}

// Feature 1: Connection queue
void connection_handler(shared_data_t* data, semaphores_t* sems) {
    //Master process accpts connections (producer)

    //Create a socket and listen for incoming connections
    int server_fd = create_server_socket(8080);
    if (server_fd < 0) {
        perror("Failed to create server socket");
        return;
    }

    while (keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("Failed to accept connection");
            continue;
        }
        enqueue_connection(data, sems, client_fd);
    }

    close(server_fd);
}

void handle_connection(int client_fd) {
    // Simple example: read data from client and echo back
    char buffer[1024];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        write(client_fd, buffer, bytes_read);
    }
    close(client_fd);
}

// Threads workers (consumers)
void* worker_thread(void* arg) {
    thread_args_t* args = (thread_args_t*)arg; //Casts the argument to the correct type
    shared_data_t* data = args->data; // Access the shared data
    semaphores_t* sems = args->sems; // Access the semaphores

    while (1) {
        int client_fd = dequeue_connection(data, sems); //Dequeue a connection
        if (client_fd == -1) { //If there are no more connections, exit the thread
            break;
        }
        handle_connection(client_fd); //Handle the connection
    }
    return NULL;
}


int main() {
    //Create shared memory
    key_t key = ftok("master.c", 'R'); //Unique key for memory
    int shmid = shmget(key, sizeof(shared_data_t), IPC_CREAT | 0666);

    if (shmid < 0) {
        perror("Failed to create shared memory");
        exit(1);
    }

    // Shared Data
    shared_data_t* data = (shared_data_t*)shmat(shmid, NULL, 0);

    if (data == (shared_data_t*) -1) {
        perror("shmat failed");
        exit(1);
    }

    // Declare semaphores
    sem_t empty_slots, filled_slots, queue_mutex;
    semaphores_t sems;
    sems.empty_slots = &empty_slots;
    sems.filled_slots = &filled_slots;
    sems.queue_mutex = &queue_mutex;

    //Initialize semaphores
    sem_init(sems.empty_slots, 0, MAX_QUEUE_SIZE);
    sem_init(sems.filled_slots, 0, 0);
    sem_init(sems.queue_mutex, 0, 1);

    //Initialize shared memory queue
    data->queue.front = 0;
    data->queue.rear = 0;
    data->queue.count = 0;


    // Declare thread arguments
    thread_args_t args;
    args.data = data;
    args.sems = &sems;

    //Declare worker thread
    pthread_t workers[MAX_WORKERS];

    // Create worker threads
    for (int i = 0; i < MAX_WORKERS; i++) {
        pthread_create(&workers[i], NULL, worker_thread, &args);
    }

    //Execute connection handler
    connection_handler(data, &sems);

    //Wait for all workers to finish
    for (int i = 0; i < MAX_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    //Cleanup semaphores
    sem_destroy(sems.empty_slots);
    sem_destroy(sems.filled_slots);
    sem_destroy(sems.queue_mutex);


    //Remove shared memory
    shmdt(data);
    shmctl(shmid, IPC_RMID, NULL);

    return 0;
}
