#ifndef TRABALHO2_SO_CACHE_H
#define TRABALHO2_SO_CACHE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Tipos opacos */
typedef struct file_cache file_cache_t;
typedef struct cache_entry cache_entry_t;

/* Handle “pinado” para impedir a expulsão enquanto está a ser usado */
typedef struct {
    const uint8_t *data;  /* ponteiro RO para o conteúdo */
    size_t size;          /* tamanho do ficheiro */
    cache_entry_t *_entry;/* uso interno */
} cache_handle_t;

/* Cria cache LRU com capacidade máxima em bytes. */
file_cache_t *cache_create(size_t capacity_bytes);

/* Destroi a cache e liberta memória. */
void cache_destroy(file_cache_t *c);

/* Tenta obter (pin) uma entrada existente. Retorna true em sucesso. */
bool cache_acquire(file_cache_t *c, const char *key, cache_handle_t *out);

/* Liberta o pin da entrada. */
void cache_release(file_cache_t *c, cache_handle_t *h);

/* Carrega um ficheiro do FS para a cache (ou reusa entrada existente).
 * key: chave lógica (ex.: path HTTP)
 * abs_path: caminho absoluto no FS
 */
bool cache_load_file(file_cache_t *c, const char *key, const char *abs_path, cache_handle_t *out);

/* Invalida uma entrada (se não estiver em uso). */
bool cache_invalidate(file_cache_t *c, const char *key);

/* Estatísticas (qualquer ponteiro pode ser NULL). */
void cache_stats(file_cache_t *c,
                 size_t *out_items,
                 size_t *out_bytes,
                 size_t *out_capacity,
                 size_t *out_hits,
                 size_t *out_misses,
                 size_t *out_evictions);

#endif /* TRABALHO2_SO_CACHE_H */
