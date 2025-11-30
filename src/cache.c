#define _POSIX_C_SOURCE 200809L // To pthread_mutexattr_settype; L -> long
#include "cache.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>

// =============================================================================
// FILE CACHE IMPLEMENTATION WITH HASH TABLE AND LRU
// =============================================================================
// This implementation provides a thread-safe file cache using:
// - A hash table for fast entry lookups.
// - A doubly linked list for LRU (Least Recently Used) management.
// - Mutex for synchronization in multi-threaded environments.
// - Reference counting to prevent removal of entries in use.
//
// Main functionalities:
// - cache_create: Creates a new cache instance.
// - cache_destroy: Destroys the cache and frees memory.
// - cache_acquire: Retrieves file data from cache (if exists).
// - cache_release: Releases a cache reference.
// - cache_load_file: Loads a file into the cache.
// - cache_invalidate: Removes an entry from the cache.
// - cache_stats: Retrieves cache statistics.

// =============================================================================
// INTERNAL STRUCTURES
// =============================================================================

// Cache Entry Structure
typedef struct cache_entry {
    char *key;              // File path (key)
    uint8_t *data;          // File data
    size_t size;            // Size in bytes
    struct cache_entry *prev, *next;  // LRU doubly linked list
    struct cache_entry *hnext;        // Next entry in hash bucket
    size_t refcnt;          // Reference count (to avoid removal while in use)
} cache_entry_t;

// File Cache Structure
// Uses hash table for fast lookups and doubly linked list for LRU
struct file_cache {
    size_t capacity;        // Maximum capacity in bytes
    size_t bytes_used;      // Currently used bytes
    size_t items;           // Current number of items

    cache_entry_t *lru_head; // LRU list head (most recent)
    cache_entry_t *lru_tail; // LRU list tail (least recent)

    size_t nbuckets;        // Number of hash buckets
    cache_entry_t **buckets; // Hash table buckets array

    pthread_mutex_t mtx;    // Mutex for thread safety

    /* Statistics */
    size_t hits, misses, evictions; // Hits, misses, evictions
};

// =============================================================================
// INTERNAL HELPER FUNCTIONS
// =============================================================================

// Simple DJB2 hash function for strings
// The DJB2 algorithm is an efficient hash function for strings.
// It starts with an initial value (5381) and iterates over each character,
// multiplying the hash by 33 (h = h*33 + c) to distribute values well.
// Returns an unsigned long used to index into the hash table.
// Parameters:
// - s: input string (key)
// Return: hash value

static unsigned long hash_key(const char *s) {
    unsigned long h = 5381UL;  // Initial value; UL -> unsigned long

    int c;  // Current character

    while ((c = (unsigned char)*s++)) {
        // h = h * 33 + c;
        h = (h << 5) + h; // h * 32 + h = h * 33
        h += (unsigned long)c;
    }
    return h;
}

static cache_entry_t *bucket_find(cache_entry_t *head, const char *key) {
    // Searches for an entry with the given key in the linked list starting from head.
    // Returns the entry if found, NULL otherwise.

    // Traverse bucket list
    for (cache_entry_t *e = head; e; e = e->hnext){

        // Compare keys
        if (strcmp(e->key, key) == 0) {
            return e;// Key match
        } 
    }
    return NULL; // Not found
}

// LRU List Management Functions
static void lru_move_front(file_cache_t *c, cache_entry_t *e) {
    // Moves the entry 'e' to the front of the LRU list (most recent).
    // This is called when an entry is accessed, making it the most recently used.

    // If already at the front, do nothing
    if (c->lru_head == e) {
        return;
    }

    // Remove from current position in the list
    if (e->prev) {
        e->prev->next = e->next; // Update previous's next
    }
    if (e->next) {
        e->next->prev = e->prev; // Update next's prev
    }
    // If this entry was the tail, update the tail pointer
    if (c->lru_tail == e) {
        c->lru_tail = e->prev; // Update tail
    }

    // Insert at the front of the list
    e->prev = NULL; // No previous
    e->next = c->lru_head; // Next is current head

    // Update old head's prev pointer
    if (c->lru_head) {
        c->lru_head->prev = e; // Old head's prev is e
    }
    c->lru_head = e; // Update head to e

    // If the list was empty before, set tail as well
    if (!c->lru_tail) {
        c->lru_tail = e; // Tail is also e
    }
}

// Inserts a new entry at the front of the LRU list
static void lru_push_front(file_cache_t *c, cache_entry_t *e) {
    // Inserts a new entry 'e' at the front of the LRU list.
    // Used when adding a new entry to the cache.
    e->prev = NULL; e->next = c->lru_head;
    if (c->lru_head) c->lru_head->prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;  // If list was empty
}

// Removes an entry from the LRU list
static void lru_remove(file_cache_t *c, cache_entry_t *e) {
    // Removes the entry 'e' from the LRU list.
    // Updates head and tail pointers as necessary.
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (c->lru_head == e) c->lru_head = e->next;
    if (c->lru_tail == e) c->lru_tail = e->prev;
    e->prev = e->next = NULL;  // Clear pointers
}

// Hash Bucket Management Functions
static void bucket_insert(file_cache_t *c, cache_entry_t *e) {
    // Inserts the entry 'e' into the appropriate hash bucket.
    // Uses the hash of the key to determine the bucket index.
    unsigned long h = hash_key(e->key) % c->nbuckets;
    e->hnext = c->buckets[h];  // Point to current head of bucket
    c->buckets[h] = e;         // Make e the new head
}

// Removes an entry from its hash bucket
static void bucket_remove(file_cache_t *c, cache_entry_t *e) {
    // Removes the entry 'e' from its hash bucket.
    // Traverses the linked list in the bucket to find and remove e.
    unsigned long h = hash_key(e->key) % c->nbuckets;
    cache_entry_t **p = &c->buckets[h];  // Pointer to head of bucket
    while (*p) {
        if (*p == e) { *p = e->hnext; e->hnext = NULL; return; }  // Remove e
        p = &(*p)->hnext;  // Move to next
    }
}

// Evicts entries from the cache until within capacity
static void evict_if_needed(file_cache_t *c) {
    // Evicts least recently used entries until capacity is not exceeded.
    // Skips entries that are currently in use (refcnt > 0).
    while (c->bytes_used > c->capacity && c->lru_tail) {
        cache_entry_t *e = c->lru_tail;  // Start from least recent
        while (e && e->refcnt > 0) e = e->prev;  // Find one not in use
        if (!e) break;  // All in use, wait for release

        bucket_remove(c, e); // Remove from hash bucket
        lru_remove(c, e);   // Remove from LRU list

        c->bytes_used -= e->size; // Update used bytes
        c->items--; // Update item count
        c->evictions++; // Update eviction count

        // Free entry memory
        free(e->data);
        free(e->key);
        free(e);
    }
}

// =============================================================================
// PUBLIC API FUNCTIONS
// =============================================================================

file_cache_t *cache_create(size_t capacity_bytes) {

    // Creates a new file cache with the specified capacity.
    // If capacity is 0, defaults to 1 MiB.
    // Allocates memory for the cache structure and hash buckets.
    // Initializes the mutex for thread safety.
    // Returns NULL on allocation failure.

    if (capacity_bytes == 0){

        // 1<<20 is 1 MiB
        capacity_bytes = 1 << 20; // Default to 1 MiB
    } 

    file_cache_t *c = calloc(1, sizeof(*c)); // Allocate cache structure
    
    if (!c){ 
        return NULL; // Allocation failure
    }

    c->capacity = capacity_bytes; // Set capacity
    
    c->nbuckets = 1024;  // Fixed number of buckets

    c->buckets = calloc(c->nbuckets, sizeof(cache_entry_t*)); // Allocate buckets

    if (!c->buckets) { // Allocation failure
        free(c);  // Free cache structure
        return NULL; // Return NULL
    } 
    pthread_mutex_init(&c->mtx, NULL); // Initialize mutex

    return c; // Return created cache
}

void cache_destroy(file_cache_t *c) {

    // Destroys the cache and frees all allocated memory.
    // Locks the mutex to ensure thread safety during destruction.
    // Frees all entries in the hash buckets and the buckets array.
    // Destroys the mutex and frees the cache structure.

    // Check for NULL cache
    if (!c){
        return;
    }

    pthread_mutex_lock(&c->mtx); // Lock mutex

    // Free all entries in all buckets
    for (size_t i = 0; i < c->nbuckets; ++i) { // For each bucket

        cache_entry_t *e = c->buckets[i]; // Get head of bucket

        // Free all entries in the bucket
        while (e) {
            // Get next entry before freeing
            cache_entry_t *n = e->hnext; // Next entry in bucket

            // Free entry memory
            free(e->data);
            free(e->key);
            free(e);

            e = n; // Move to next entry
        }
    }

    pthread_mutex_unlock(&c->mtx); // Unlock mutex
    pthread_mutex_destroy(&c->mtx); // Destroy mutex

    // Free buckets array and cache structure
    free(c->buckets);
    free(c);
}

bool cache_acquire(file_cache_t *c, const char *key, cache_handle_t *out) {
    // Attempts to acquire a cache entry for the given key.
    // If the entry exists:
    //   - Moves it to the front of the LRU list (most recently used).
    //   - Increments its reference count (refcnt).
    //   - Fills the output handle with entry data and size.
    //   - Increments the cache hit counter.
    // If not found:
    //   - Increments the cache miss counter.
    //   - Returns false.
    // Thread-safe: locks the cache mutex during the operation.

    // Validate input parameters
    if (!c || !key || !out) {
        return false;
    }

    pthread_mutex_lock(&c->mtx); // Lock cache for thread safety

    // Compute hash bucket index for the key
    unsigned long h = hash_key(key) % c->nbuckets;

    // Search for the entry in the hash bucket
    cache_entry_t *e = bucket_find(c->buckets[h], key);

    if (!e) {
        // Entry not found: increment miss counter and return false
        c->misses++;
        pthread_mutex_unlock(&c->mtx);
        return false;
    }

    // Entry found: move to front of LRU list (most recently used)
    lru_move_front(c, e);

    // Increment reference count to mark as in-use
    e->refcnt++;

    // Fill output handle with entry data
    out->data = e->data;
    out->size = e->size;
    out->_entry = e;

    // Increment hit counter
    c->hits++;

    pthread_mutex_unlock(&c->mtx); // Unlock cache

    return true;
}


/*
 * Releases a reference to a cache entry acquired via cache_acquire.
 * Decrements the reference count. If capacity is exceeded after release,
 * triggers LRU eviction. Clears the handle to prevent reuse.
 * Thread-safe with mutex.
 */

void cache_release(file_cache_t *c, cache_handle_t *h) {

    // Validate input parameters
    if (!c || !h || !h->_entry)
        return;

    pthread_mutex_lock(&c->mtx); // Lock cache for thread safety

    cache_entry_t *e = (cache_entry_t*)h->_entry; // Get entry from handle

    // Decrement reference count if greater than zero
    if (e->refcnt > 0){
        e->refcnt--; // Decrement ref count
    }

    // Clear handle to prevent accidental reuse
    h->_entry = NULL; // Clear internal entry pointer
    h->data = NULL; // Clear data pointer
    h->size = 0; // Clear size

    // Evict if needed after release
    if (c->bytes_used > c->capacity){
        evict_if_needed(c); // Evict entries if over capacity
    }

    // Unlock cache
    pthread_mutex_unlock(&c->mtx);
}

/*
 * Reads the entire file at abs_path into memory.
 * Allocates a buffer, reads the file, and sets data and size.
 * Returns true on success, false on failure (file not found, read error, etc.).
 */
static bool read_file_into_memory(const char *abs_path, uint8_t **data, size_t *size) {
    *data = NULL; // Initialize output data pointer
    *size = 0; // Initialize output size

    // Open file for reading in binary mode
    FILE *f = fopen(abs_path, "rb");

    // Check if file opened successfully
    if (!f)
        return false;

    // Determine file size
    // fseek -> Move to end of file
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }

    // Get current position (file size)
    // ftell -> Get current file position
    long len = ftell(f);

    // Check for errors
    if (len < 0) {
        fclose(f); // Close file
        return false;
    }


    // Rewind to beginning of file
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }


    // Allocate buffer for file data with size len
    uint8_t *buf = (uint8_t*)malloc((size_t)len);

    // Check for allocation failure
    if (!buf) {
        fclose(f);
        return false;
    }

    // Read file data into buffer
    // fread -> Read data from file
    size_t rd = fread(buf, 1, (size_t)len, f);

    fclose(f);

    // Check if read size matches expected length
    if (rd != (size_t)len) {
        free(buf);
        return false;
    }

    // Set output parameters
    *data = buf; // Set data pointer
    *size = (size_t)len; // Set size

    return true; // Success
}

/*
 * Loads a file into the cache if not already present.
 * First checks if the key is already in cache (via cache_acquire).
 * If not, reads the file from disk, creates a new entry, inserts into hash and LRU,
 * and evicts if necessary. Increments refcnt and fills output handle.
 * Thread-safe with mutex.
 */
bool cache_load_file(file_cache_t *c, const char *key, const char *abs_path, cache_handle_t *out) {

    // Validate input parameters
    if (!c || !key || !abs_path || !out)
        return false;

    // Try to acquire from cache first (fast path)
    // cache_acquire will handle locking
    if (cache_acquire(c, key, out))
        return true;

    // Not in cache: read file from disk
    uint8_t *buf = NULL; // Buffer for file data
    size_t sz = 0; // Size of file data

    // Read file into memory
    if (!read_file_into_memory(abs_path, &buf, &sz))
        return false;

    pthread_mutex_lock(&c->mtx); // Lock cache for thread safety

    // Double-check if another thread loaded it while we were reading
    unsigned long h = hash_key(key) % c->nbuckets; // Hash bucket index
    cache_entry_t *e = bucket_find(c->buckets[h], key); // Search for entry

    // If found, reuse existing entry
    if (e) {
        // Already loaded by another thread

        lru_move_front(c, e); // Move to front of LRU

        e->refcnt++; // Increment ref count

        // Fill output handle
        out->data = e->data; 
        out->size = e->size; 
        out->_entry = e; 

        c->hits++; // Increment hit counter

        pthread_mutex_unlock(&c->mtx); // Unlock cache

        free(buf);  // Free the buffer we read

        return true;
    }

    // Create new entry for this file
    e = (cache_entry_t*)calloc(1, sizeof(*e)); // Allocate entry

    // Check for allocation failure
    if (!e) {
        pthread_mutex_unlock(&c->mtx); // Unlock cache
        free(buf); // Free file buffer
        return false;
    }

    // strdup duplicates the string (allocates memory and copies)
    e->key = strdup(key); // Duplicate key string

    // Check for strdup failure
    if (!e->key) {
        pthread_mutex_unlock(&c->mtx); // Unlock cache

        free(buf); // Free file buffer
        free(e); // Free entry
        
        return false; // Return failure
    }

    e->data = buf; // Set file data
    e->size = sz; // Set file size
    e->refcnt = 1; // Set initial ref count

    // Insert into hash table and LRU list
    bucket_insert(c, e); // Insert into hash bucket
    lru_push_front(c, e); // Insert into LRU list

    c->items++; // Increment item count
    c->bytes_used += sz; // Update used bytes

    // Evict if over capacity
    evict_if_needed(c);

    // Fill output handle
    out->data = e->data;
    out->size = e->size;
    out->_entry = e;

    pthread_mutex_unlock(&c->mtx);
    return true;
}

/*
 * Removes the entry with the given key from the cache.
 * Only succeeds if the entry exists and is not currently in use (refcnt == 0).
 * Frees the associated memory and updates statistics.
 * Thread-safe with mutex.
 */
bool cache_invalidate(file_cache_t *c, const char *key) {

    // Validate input parameters
    if (!c || !key)
        return false;

    pthread_mutex_lock(&c->mtx); // Lock cache for thread safety

    // Find the entry in the hash bucket
    unsigned long h = hash_key(key) % c->nbuckets; // Hash bucket index

    // Search for the entry
    cache_entry_t *e = bucket_find(c->buckets[h], key); // Find entry

    // If not found, return false
    if (!e) {
        pthread_mutex_unlock(&c->mtx); // Unlock cache
        return false;
    }

    // Only invalidate if not in use
    if (e->refcnt > 0) {
        pthread_mutex_unlock(&c->mtx); // Unlock cache
        return false;
    }

    bucket_remove(c, e); // Remove from hash bucket
    lru_remove(c, e); // Remove from LRU list

    c->bytes_used -= e->size; // Update used bytes
    c->items--; // Update item count

    // Free entry memory
    free(e->data);
    free(e->key);
    free(e);

    pthread_mutex_unlock(&c->mtx); // Unlock cache

    return true;
}

/*
 * Retrieves current cache statistics.
 * Copies the values to the provided output pointers if not NULL.
 * Thread-safe with mutex.
 */
void cache_stats(
    file_cache_t *c, // Cache instance
    size_t *out_items, // Number of items
    size_t *out_bytes, // Used bytes
    size_t *out_capacity, // Capacity in bytes
    size_t *out_hits, // Cache hits
    size_t *out_misses, // Cache misses
    size_t *out_evictions // Cache evictions
) {

    // Validate cache parameter
    if (!c)
        return;

    pthread_mutex_lock(&c->mtx); // Lock cache for thread safety


    // Copy statistics to output parameters if not NULL
    if (out_items)
        *out_items = c->items; // Number of items

    if (out_bytes)
        *out_bytes = c->bytes_used;  // Used bytes

    if (out_capacity)
        *out_capacity = c->capacity; // Capacity in bytes

    if (out_hits)
        *out_hits = c->hits; // Cache hits

    if (out_misses)
        *out_misses = c->misses; // Cache misses

    if (out_evictions)
        *out_evictions = c->evictions; // Cache evictions

    pthread_mutex_unlock(&c->mtx); // Unlock cache
}
