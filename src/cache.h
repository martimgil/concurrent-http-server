#ifndef TRABALHO2_SO_CACHE_H
#define TRABALHO2_SO_CACHE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Opaque types */
typedef struct file_cache file_cache_t; // File cache structure
typedef struct cache_entry cache_entry_t; // Cache entry structure

/* Handle "pinned" to prevent eviction while in use */
typedef struct {
    const uint8_t *data;    /* Read-only pointer to file contents */
    size_t size;            /* File size in bytes */
    cache_entry_t *_entry;  /* Internal use only */
} cache_handle_t;

/* Create an LRU cache with a maximum capacity in bytes */
file_cache_t *cache_create(size_t capacity_bytes);

/* Destroy the cache and free all memory */
void cache_destroy(file_cache_t *cache);

/* Try to acquire (pin) an existing entry. Returns true on success */
bool cache_acquire(file_cache_t *cache, const char *key, cache_handle_t *out);

/* Release the pin on an entry */
void cache_release(file_cache_t *cache, cache_handle_t *handle);

/* Load a file from the filesystem into the cache (or reuse existing entry)
 * key: logical key (e.g., HTTP path)
 * abs_path: absolute filesystem path
 */
bool cache_load_file(file_cache_t *cache, const char *key, const char *abs_path, cache_handle_t *out);

/* Invalidate an entry (if not in use). Returns true if invalidated */
bool cache_invalidate(file_cache_t *cache, const char *key);

/* Cache statistics (any pointer can be NULL) */
void cache_stats(
    file_cache_t *cache, // Cache instance
    size_t *out_items, // Number of items
    size_t *out_bytes, // Used bytes
    size_t *out_capacity, // Capacity in bytes
    size_t *out_hits, // Cache hits
    size_t *out_misses, // Cache misses
    size_t *out_evictions // Cache evictions
);

#endif 
