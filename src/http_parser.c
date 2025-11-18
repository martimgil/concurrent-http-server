// Providencia uma função que faz parse a primeira linha de um request HTTP
// Extrai o método, caminho, versão do buffer e guarda em http_request_t.
// Usado para interpretar os dados recebidos do cliente.

#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef struct {
    char method[16];
    char path[512];
    char version[16];
} http_request_t;

int parse_http_request(const char* buffer, http_request_t* req) {
    if (!buffer || !req) return -1;

    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) return -1;

    char first_line[1024];
    size_t len = line_end - buffer;
    if (len >= sizeof(first_line)) return -1;

    strncpy(first_line, buffer, len);
    first_line[len] = '\0';

    // Use sscanf with width limits to prevent buffer overflows
    if (sscanf(first_line, "%15s %511s %15s", req->method, req->path, req->version) != 3) {
        return -1;
    }

    return 0;
}

