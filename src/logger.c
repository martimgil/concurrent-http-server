#define _GNU_SOURCE
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <semaphore.h>

// #########################################################################################################
// LOGGER INTERNAL STATE
// #########################################################################################################

// Absolute path to the log file
static char g_log_path[512];

// File descriptor for writing
static int g_log_fd = -1;

// Global POSIX semaphore for mutual exclusion between processes & threads
static sem_t* g_sem = NULL;

// Fixed name for the POSIX semaphore (must start with '/')
static const char* LOG_SEM_NAME = "/ws_log_sem";

// Maximum allowed log file size before rotation (10 MB)
static const off_t LOG_MAX_SIZE = 10 * 1024 * 1024;

// Number of rotated log files to keep
static const int LOG_MAX_ROTATIONS = 5;

// Log buffer (Feature 5: Buffering)
#define LOG_BUFFER_SIZE 4096
static char g_log_buffer[LOG_BUFFER_SIZE];
static int g_buf_pos = 0;
static time_t g_last_flush = 0;

// #########################################################################################################
// Internal function: Get current file size
// #########################################################################################################
static off_t get_file_size(int fd) { 
    struct stat st;
    if (fstat(fd, &st) == 0) {
        return st.st_size;
    }
    return -1;
}


// #########################################################################################################
// Internal function: Perform automatic log rotation
// access.log → access.log.1 → ... → access.log.5
// #########################################################################################################

// Rotates log files when the main log file exceeds the maximum size.
static void rotate_logs() {
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }

    char oldpath[600];
    char newpath[600];

    // Remove the oldest rotated log file
    snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, LOG_MAX_ROTATIONS);
    unlink(oldpath);

    // Shift rotated logs: N-1 → N, ..., 1 → 2
    for (int i = LOG_MAX_ROTATIONS - 1; i >= 1; i--) {
        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, i);
        snprintf(newpath, sizeof(newpath), "%s.%d", g_log_path, i + 1);
        rename(oldpath, newpath);
    }

    // Move main log file to .1
    snprintf(newpath, sizeof(newpath), "%s.1", g_log_path);
    rename(g_log_path, newpath);

    // Reopen main log file (empty)
    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
}


// #########################################################################################################
// Logger initialization
// #########################################################################################################
void logger_init(const char* logfile_path) {
    strncpy(g_log_path, logfile_path, sizeof(g_log_path) - 1);
    g_log_path[sizeof(g_log_path) - 1] = '\0';

    g_sem = sem_open(LOG_SEM_NAME, O_CREAT, 0666, 1);
    if (g_sem == SEM_FAILED) {
        perror("logger: sem_open");
        exit(1);
    }

    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (g_log_fd < 0) {
        perror("logger: open logfile");
        exit(1);
    }
    
    g_buf_pos = 0;
    g_last_flush = time(NULL);
}


// #########################################################################################################
// Flush buffer to disk (Internal implementation, called with lock held)
// #########################################################################################################
static void flush_buffer_locked() {
    if (g_buf_pos > 0 && g_log_fd >= 0) {
        if (write(g_log_fd, g_log_buffer, g_buf_pos) < 0) {
            perror("logger: write failed");
        }
        g_buf_pos = 0;
        g_last_flush = time(NULL);
    }
}

// Public flush function (Thread-safe)
void logger_flush() {
    if (!g_sem || g_log_fd < 0) return;
    
    sem_wait(g_sem);
    flush_buffer_locked();
    sem_post(g_sem);
}

// #########################################################################################################
// Logger shutdown
// #########################################################################################################
void logger_close() { 
    if (g_sem) {
        sem_wait(g_sem);
        flush_buffer_locked(); // Ensure any remaining logs are written
        sem_post(g_sem);
    }

    if (g_log_fd >= 0) { 
        close(g_log_fd);
        g_log_fd = -1;
    }

    if (g_sem) {
        sem_close(g_sem);
    }
}

// #########################################################################################################
// Write a log entry (with semaphore + buffer + automatic rotation)
// #########################################################################################################
void logger_write(
    const char* ip, 
    const char* method, 
    const char* path, 
    int status, 
    size_t bytes_sent, 
    long duration_ms
) {
    if (!g_sem || g_log_fd < 0) {
        return;
    }

    // Enter critical section
    sem_wait(g_sem); 

    // Rotate if needed (check size before writing)
    off_t size_now = get_file_size(g_log_fd);
    if (size_now >= LOG_MAX_SIZE) {
        flush_buffer_locked(); // Flush before rotating
        rotate_logs();
    }

    // Format log line
    char datebuf[64];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(datebuf, sizeof(datebuf), "%d/%b/%Y:%H:%M:%S", &tm_now);

    char line[1024];
    int len = snprintf( 
        line,
        sizeof(line),
        "%s [%s] \"%s %s\" %d %zu %ldms\n",
        ip,
        datebuf,
        method,
        path,
        status,
        bytes_sent,
        duration_ms
    );

    if (len > 0 && (size_t)len < sizeof(line)) {
        // If line is too big for buffer, flush first
        if (g_buf_pos + len >= LOG_BUFFER_SIZE) {
             flush_buffer_locked();
        }

        // Append to buffer (will always fit after flush, since len < 1024 < 4096)
        memcpy(g_log_buffer + g_buf_pos, line, (size_t)len);
        g_buf_pos += len;
        
        // Time-based flush: Only flush if 5 seconds have passed since last flush
        if (now - g_last_flush >= 5) {
             flush_buffer_locked();
        }
    }

    // Leave critical section
    sem_post(g_sem);
}