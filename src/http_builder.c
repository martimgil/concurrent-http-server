// Providencia uma função para construir e enviar uma resposta HTTP sobre um socket.
// Fotmata o HTTP header e envia a resposta completa (header + body) através do socket fornecido.

#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

void send_http_response(int fd, int status, const char* status_msg,
const char* content_type, const char* body, size_t body_len) {
    if (fd < 0 || !status_msg || !content_type) {
        return;
    }

    char header[2048];
    int header_len = snprintf(header, sizeof(header),
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %zu\r\n"
    "Server: ConcurrentHTTP/1.0\r\n"
    "Connection: close\r\n"
    "\r\n",
    status, status_msg, content_type, body_len);

    if (header_len < 0 || header_len >= (int)sizeof(header)) {
        perror("Header formatting failed");
        return;
    }

    ssize_t sent = send(fd, header, header_len, 0);
    if (sent < 0) {
        perror("Failed to send header");
        return;
    }

    if (body && body_len > 0) {
        sent = send(fd, body, body_len, 0);
        if (sent < 0) {
            perror("Failed to send body");
        }
    }
}

