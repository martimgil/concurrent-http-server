#define _POSIX_C_SOURCE 200809L
#include "cache.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

/* ---------- Estruturas: Hash (buckets) + LRU (DLL) ---------- */

typedef struct cache_entry {
    char *key;
    uint8_t *data;
    size_t size;

    struct cache_entry *prev, *next;  /* LRU */
    struct cache_entry *hnext;        /* hash encadeado */

    size_t refcnt; /* nº de “pins” ativos */
} cache_entry_t;

struct file_cache {
    size_t capacity;   /* limite em bytes */
    size_t bytes_used; /* bytes ocupados */
    size_t items;

    cache_entry_t *lru_head; /* mais recente */
    cache_entry_t *lru_tail; /* menos recente */

    size_t nbuckets;
    cache_entry_t **buckets;

    pthread_mutex_t mtx;

    /* stats */
    size_t hits, misses, evictions;
};

/* djb2 */
static unsigned long hash_key(const char *s) {
    unsigned long h = 5381UL; int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static cache_entry_t *bucket_find(cache_entry_t *head, const char *key) {
    for (cache_entry_t *e = head; e; e = e->hnext)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

static void lru_move_front(file_cache_t *c, cache_entry_t *e) {
    if (c->lru_head == e) return;
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (c->lru_tail == e) c->lru_tail = e->prev;
    e->prev = NULL; e->next = c->lru_head;
    if (c->lru_head) c->lru_head->prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

static void lru_push_front(file_cache_t *c, cache_entry_t *e) {
    e->prev = NULL; e->next = c->lru_head;
    if (c->lru_head) c->lru_head->prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

static void lru_remove(file_cache_t *c, cache_entry_t *e) {
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (c->lru_head == e) c->lru_head = e->next;
    if (c->lru_tail == e) c->lru_tail = e->prev;
    e->prev = e->next = NULL;
}

static void bucket_insert(file_cache_t *c, cache_entry_t *e) {
    unsigned long h = hash_key(e->key) % c->nbuckets;
    e->hnext = c->buckets[h];
    c->buckets[h] = e;
}

static void bucket_remove(file_cache_t *c, cache_entry_t *e) {
    unsigned long h = hash_key(e->key) % c->nbuckets;
    cache_entry_t **p = &c->buckets[h];
    while (*p) {
        if (*p == e) { *p = e->hnext; e->hnext = NULL; return; }
        p = &(*p)->hnext;
    }
}

/* Expulsão LRU (ignora entradas com refcnt>0). */
static void evict_if_needed(file_cache_t *c) {
    while (c->bytes_used > c->capacity && c->lru_tail) {
        cache_entry_t *e = c->lru_tail;
        while (e && e->refcnt > 0) e = e->prev;
        if (!e) break; /* todas em uso, esperar release */

        bucket_remove(c, e);
        lru_remove(c, e);

        c->bytes_used -= e->size;
        c->items--;
        c->evictions++;

        free(e->data);
        free(e->key);
        free(e);
    }
}

file_cache_t *cache_create(size_t capacity_bytes) {
    if (capacity_bytes == 0) capacity_bytes = 1 << 20; /* 1 MiB */
    file_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->capacity = capacity_bytes;
    c->nbuckets = 1024;
    c->buckets = calloc(c->nbuckets, sizeof(cache_entry_t*));
    if (!c->buckets) { free(c); return NULL; }
    pthread_mutex_init(&c->mtx, NULL);
    return c;
}

void cache_destroy(file_cache_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mtx);
    for (size_t i = 0; i < c->nbuckets; ++i) {
        cache_entry_t *e = c->buckets[i];
        while (e) {
            cache_entry_t *n = e->hnext;
            free(e->data);
            free(e->key);
            free(e);
            e = n;
        }
    }
    pthread_mutex_unlock(&c->mtx);
    pthread_mutex_destroy(&c->mtx);
    free(c->buckets);
    free(c);
}

bool cache_acquire(file_cache_t *c, const char *key, cache_handle_t *out) {
    if (!c || !key || !out) return false;
    pthread_mutex_lock(&c->mtx);
    unsigned long h = hash_key(key) % c->nbuckets;
    cache_entry_t *e = bucket_find(c->buckets[h], key);
    if (!e) { c->misses++; pthread_mutex_unlock(&c->mtx); return false; }
    lru_move_front(c, e);
    e->refcnt++;
    out->data = e->data; out->size = e->size; out->_entry = e;
    c->hits++;
    pthread_mutex_unlock(&c->mtx);
    return true;
}

void cache_release(file_cache_t *c, cache_handle_t *h) {
    if (!c || !h || !h->_entry) return;
    pthread_mutex_lock(&c->mtx);
    cache_entry_t *e = (cache_entry_t*)h->_entry;
    if (e->refcnt > 0) e->refcnt--;
    h->_entry = NULL; h->data = NULL; h->size = 0;
    if (c->bytes_used > c->capacity) evict_if_needed(c);
    pthread_mutex_unlock(&c->mtx);
}

static bool read_file_into_memory(const char *abs_path, uint8_t **data, size_t *size) {
    *data = NULL; *size = 0;
    FILE *f = fopen(abs_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t *buf = (uint8_t*)malloc((size_t)len);
    if (!buf) { fclose(f); return false; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (rd != (size_t)len) { free(buf); return false; }
    *data = buf; *size = (size_t)len;
    return true;
}

bool cache_load_file(file_cache_t *c, const char *key, const char *abs_path, cache_handle_t *out) {
    if (!c || !key || !abs_path || !out) return false;
    if (cache_acquire(c, key, out)) return true;

    uint8_t *buf = NULL; size_t sz = 0;
    if (!read_file_into_memory(abs_path, &buf, &sz)) return false;

    pthread_mutex_lock(&c->mtx);

    unsigned long h = hash_key(key) % c->nbuckets;
    cache_entry_t *e = bucket_find(c->buckets[h], key);
    if (e) {
        lru_move_front(c, e);
        e->refcnt++;
        out->data = e->data; out->size = e->size; out->_entry = e;
        c->hits++;
        pthread_mutex_unlock(&c->mtx);
        free(buf);
        return true;
    }

    e = (cache_entry_t*)calloc(1, sizeof(*e));
    if (!e) { pthread_mutex_unlock(&c->mtx); free(buf); return false; }
    e->key = strdup(key);
    if (!e->key) { pthread_mutex_unlock(&c->mtx); free(buf); free(e); return false; }
    e->data = buf; e->size = sz; e->refcnt = 1;

    bucket_insert(c, e);
    lru_push_front(c, e);
    c->items++; c->bytes_used += sz;

    evict_if_needed(c);

    out->data = e->data; out->size = e->size; out->_entry = e;

    pthread_mutex_unlock(&c->mtx);
    return true;
}

bool cache_invalidate(file_cache_t *c, const char *key) {
    if (!c || !key) return false;
    pthread_mutex_lock(&c->mtx);
    unsigned long h = hash_key(key) % c->nbuckets;
    cache_entry_t *e = bucket_find(c->buckets[h], key);
    if (!e) { pthread_mutex_unlock(&c->mtx); return false; }
    if (e->refcnt > 0) { pthread_mutex_unlock(&c->mtx); return false; }
    bucket_remove(c, e);
    lru_remove(c, e);
    c->bytes_used -= e->size;
    c->items--;
    free(e->data);
    free(e->key);
    free(e);
    pthread_mutex_unlock(&c->mtx);
    return true;
}

void cache_stats(file_cache_t *c,
                 size_t *out_items,
                 size_t *out_bytes,
                 size_t *out_capacity,
                 size_t *out_hits,
                 size_t *out_misses,
                 size_t *out_evictions) {
    if (!c) return;
    pthread_mutex_lock(&c->mtx);
    if (out_items)     *out_items = c->items;
    if (out_bytes)     *out_bytes = c->bytes_used;
    if (out_capacity)  *out_capacity = c->capacity;
    if (out_hits)      *out_hits = c->hits;
    if (out_misses)    *out_misses = c->misses;
    if (out_evictions) *out_evictions = c->evictions;
    pthread_mutex_unlock(&c->mtx);
}
