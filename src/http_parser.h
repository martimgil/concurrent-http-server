#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdbool.h>

// Função para verificar e sanitizar o pedido GET
bool parse_http_request(const char* request, char* method, char* path, size_t path_size);

#endif /* HTTP_PARSER_H */
