#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>

// Implementa a logica principal de aceitar conexoes do cliente.
// É aqui a parte central da arquitetira multi-thread do servidor.
#DEFINE PORT 8080
#define MAX_QUEUE_SIZE 1024
#define MAX_WORKERS 4

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

void enqueue_connection(shared_data_t* data, semaphores_t* sems, int
client_fd) {
    sem_wait(sems->empty_slots);
    sem_wait(sems->queue_mutex);

    data->queue.sockets[data->queue.rear] = client_fd;
    data->queue.rear = (data->queue.rear + 1) % MAX_QUEUE_SIZE;
    data->queue.count++;

    sem_post(sems->queue_mutex);
    sem_post(sems->filled_slots);
}

int dequeue_connection(shared_data_t* data, semaphores_t* sems){
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

void connection_handler(shared_data_t* data, semaphores_t* sems){
    //Master process accpts connections (producer)

    //Create a socket and listen for incoming connections
    int server_fd = create_server_socket(8080);
    if(server_fd <0){
        perror("Failed to create server socket");}
        return;
    }

    while(keep_running){
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("Failed to accept connection");
        continue;
        }
        enqueue_connection(data, sems, client_fd);
    }

    close(server_fd);
}


// Threads workers (consumers)
void* worker_thread(void* arg){
	thread_args_t* args = (thread_args_t*)arg; //Casts the argument to the correct type
	shared_data_t* data = targs->data; // Access the shared data
	semaphores_t* sems = targs->sems; // Access the semaphores


	while(1){
		int client_fd = dequeue_connection(data, sems); //Dequeue a connection
		if (client_fd == -1){ //If there are no more connections, exit the thread
			break;
		}
		handle_connection(client_fd); //Handle the connection
	}
	return NULL;
}


