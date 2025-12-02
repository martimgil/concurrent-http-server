#include "../src/cache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

// Test data
#define TEST_FILE_CONTENT "This is test content for cache consistency test.\n"
#define TEST_KEY "test_file.txt"
#define NUM_THREADS 10
#define NUM_ITERATIONS 100

// Global cache
file_cache_t *g_cache;

// Thread function to test cache consistency
void* cache_test_thread(void *arg) {
    int thread_id = *(int*)arg;
    char expected_content[] = TEST_FILE_CONTENT;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        cache_handle_t h;

        // Try to acquire from cache
        if (cache_acquire(g_cache, TEST_KEY, &h)) {
            // Verify content
            if (h.size != strlen(expected_content) ||
                memcmp(h.data, expected_content, h.size) != 0) {
                fprintf(stderr, "Thread %d: Cache content mismatch at iteration %d\n", thread_id, i);
                exit(1);
            }
            cache_release(g_cache, &h);
        } else {
            // Load into cache if not present
            if (!cache_load_file(g_cache, TEST_KEY, TEST_KEY, &h)) {
                fprintf(stderr, "Thread %d: Failed to load file at iteration %d\n", thread_id, i);
                exit(1);
            }
            // Verify content
            if (h.size != strlen(expected_content) ||
                memcmp(h.data, expected_content, h.size) != 0) {
                fprintf(stderr, "Thread %d: Loaded content mismatch at iteration %d\n", thread_id, i);
                exit(1);
            }
            cache_release(g_cache, &h);
        }

        // Small delay to increase chance of race conditions
        usleep(100);
    }

    return NULL;
}

int main() {
    // Create a temporary test file
    FILE *f = fopen(TEST_KEY, "w");
    if (!f) {
        perror("Failed to create test file");
        return 1;
    }
    fprintf(f, "%s", TEST_FILE_CONTENT);
    fclose(f);

    // Create cache
    g_cache = cache_create(1024 * 1024); // 1 MB
    if (!g_cache) {
        fprintf(stderr, "Failed to create cache\n");
        return 1;
    }

    // Create threads
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, cache_test_thread, &thread_ids[i]) != 0) {
            perror("Failed to create thread");
            return 1;
        }
    }

    // Wait for threads
    for (int i = 0; i < NUM_THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("Failed to join thread");
            return 1;
        }
    }

    // Get cache stats
    size_t items, bytes, capacity, hits, misses, evictions;
    cache_stats(g_cache, &items, &bytes, &capacity, &hits, &misses, &evictions);

    printf("Cache test completed successfully!\n");
    printf("Items: %zu, Bytes: %zu, Capacity: %zu\n", items, bytes, capacity);
    printf("Hits: %zu, Misses: %zu, Evictions: %zu\n", hits, misses, evictions);

    // Verify that we have at least some hits (indicating cache is working)
    if (hits == 0) {
        fprintf(stderr, "No cache hits - cache may not be working properly\n");
        return 1;
    }

    // Cleanup
    cache_destroy(g_cache);
    unlink(TEST_KEY);

    printf("Cache consistency test passed.\n");
    return 0;
}
