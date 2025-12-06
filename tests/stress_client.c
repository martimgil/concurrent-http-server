#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

// Helper to open many connections and hold them
// Usage: ./stress_client <ip> <port> <num_connections> <duration_sec>

volatile int running = 1;

void handle_sig(int sig) {
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ip> <port> <num_connections> <duration>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int num_conns = atoi(argv[3]);
    int duration = atoi(argv[4]);

    if(num_conns > 1000) num_conns = 1000; // sanity limit

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        return 1;
    }

    int *sockets = malloc(sizeof(int) * num_conns);
    int connected = 0;

    signal(SIGINT, handle_sig);
    signal(SIGTERM, handle_sig);

    printf("Connecting %d clients to %s:%d...\n", num_conns, ip, port);

    for (int i = 0; i < num_conns; i++) {
        // Create socket
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("socket");
            continue;
        }

        // Connect
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            // Only whine if it's not normal after saturation
            if (i < 100) perror("connect"); 
            close(sock);
            sockets[i] = -1;
            continue; 
        }

        sockets[i] = sock;
        connected++;

        // Send a partial header to keep it active but engaged
        const char *partial = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1000000\r\n\r\n";
        write(sock, partial, strlen(partial));

        // Small delay every 10 connections to avoid SYN flooding protection
        if(i % 50 == 0) usleep(10000); 
    }

    printf("Established %d connections. Sleeping for %d seconds...\n", connected, duration);

    // Sleep in chunks to allow signal interruption
    for(int i=0; i<duration && running; i++) {
        sleep(1);
    }

    printf("Closing connections...\n");
    for (int i = 0; i < num_conns; i++) {
        if (sockets[i] != -1) {
            close(sockets[i]);
        }
    }
    free(sockets);
    return 0;
}
