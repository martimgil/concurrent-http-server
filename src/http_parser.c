// Providencia uma função que faz parse a primeira linha de um request HTTP
// Extrai o método, caminho, versão do buffer e guarda em http_request_t.
// Usado para interpretar os dados recebidos do cliente.

typedef struct {
    char method[16];
    char path[512];
    char version[16];
} http_request_t;

int parse_http_request(const char* buffer, http_request_t* req) {
    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) return -1;

    char first_line[1024];
    size_t len = line_end - buffer;
    strncpy(first_line, buffer, len);
    first_line[len] = '\0';

    if (sscanf(first_line, "%s %s %s", req->method, req->path, req->version)
   != 3) {
        return -1;
   }

    return 0;
}