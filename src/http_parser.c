#include <stdio.h>
#include <string.h>
#include <stddef.h>

// Estrutura para armazenar os componentes do pedido HTTP
typedef struct {
    char method[16];   // Método HTTP (ex.: GET, POST)
    char path[512];    // Caminho do pedido (ex.: /index.html)
    char version[16];  // Versão HTTP (ex.: HTTP/1.1)
} http_request_t;  // Estrutura do pedido HTTP


// Função para analisar um pedido HTTP do buffer
// Arguments:
//  buffer -> Ponteiro para o buffer contendo o pedido HTTP
//  req    -> Ponteiro para a estrutura http_request_t para armazenar os componentes analisados
int parse_http_request(const char* buffer, http_request_t* req) {

    // Validar parâmetros de entrada
    if (!buffer || !req) {
        return -1;  // Erro se os parâmetros forem inválidos
    }

    // Extrair a primeira linha do pedido HTTP
    // Procurar pelo fim da primeira linha (usando "\r\n")
    char* line_end = strstr(buffer, "\r\n");

    // Verificar se encontramos o fim da linha
    if (!line_end) {
        return -1;  // Retorna erro se não encontrar o fim da linha
    }

    // Copiar a primeira linha para um buffer temporário
    char first_line[1024];

    // Calcular o comprimento da primeira linha
    size_t len = line_end - buffer;

    // Garantir que o comprimento não ultrapasse o tamanho do buffer
    if (len >= sizeof(first_line)) {
        return -1;  // Erro caso o comprimento seja maior que o buffer
    }

    // Copiar a primeira linha para o buffer temporário
    strncpy(first_line, buffer, len);
    
    first_line[len] = '\0';  // Adicionar o terminador NUL para garantir uma string válida

    // Usar sscanf para fazer o parsing da linha
    // Limitar a largura dos campos para evitar estouros de buffer
    if (sscanf(first_line, "%15s %511s %15s", req->method, req->path, req->version) != 3) {
        return -1;  // Retorna erro se não conseguir analisar corretamente
    }

    // Se o método for POST, podemos adicionar mais validações (opcional)
    // Neste caso, o servidor só lida com GET, mas o POST poderia ser adicionado para outra funcionalidade
    if (strncmp(req->method, "GET", 3) != 0) {
        return -1;  // Só aceitamos o método GET
    }

    // Sanitizar o path (remove "/" extra, se necessário)
    if (req->path[0] == '/') {
        memmove(req->path, req->path + 1, strlen(req->path));  // Remover a barra inicial
    }

    // Garantir que o caminho não exceda o limite do buffer
    if (strlen(req->path) >= sizeof(req->path)) {
        return -1;  // Retorna erro se o path for muito longo
    }

    return 0;  // Retorna sucesso
}
