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

// #########################################################################################################
// Internal robust function: write all bytes reliably (handling errors and partial writes)
// #########################################################################################################
/**
 * Writes the entire buffer to the given file descriptor, handling partial writes and interruptions.
 * 
 * fd ->  File descriptor to write to.
 * buf -> Pointer to the buffer containing data to write.
 * len -> Number of bytes to write from the buffer.
 * return 0 on success (all bytes written), -1 on error.
 */
static int write_all(int fd, const void* buf, size_t len)
{
    const char* ptr = (const char*)buf; // Pointer to current position in buffer
    size_t total_written = 0;           // Total bytes written so far

    while (total_written < len) {
        // Attempt to write the remaining bytes
        ssize_t bytes_written = write(fd, ptr + total_written, len - total_written);

        if (bytes_written > 0) {
            // Successfully wrote some bytes; advance the counter
            total_written += (size_t)bytes_written;
        } else if (bytes_written == -1 && errno == EINTR) {
            // Write was interrupted by a signal; retry
            continue;
        } else {
            // Permanent error or write returned 0 (should not happen for regular files)
            return -1;
        }
    }

    // All bytes written successfully
    return 0;
}

// #########################################################################################################
// Internal function: Get current file size
// #########################################################################################################
static off_t get_file_size(int fd) { 

    struct stat st; // File status structure

    // Get file status
    // fstat -> Get file status
    if (fstat(fd, &st) == 0) {
        return st.st_size; // Return file size
    }
    return -1; // Error case
}


// #########################################################################################################
// Internal function: Perform automatic log rotation
// access.log → access.log.1 → ... → access.log.5
// #########################################################################################################

// Rotates log files when the main log file exceeds the maximum size.
static void rotate_logs() {

    // Close current log file before rotating

    if (g_log_fd >= 0) { // If log file is open
        close(g_log_fd); // Close file
        g_log_fd = -1; // Invalidate file descriptor
    }

    char oldpath[600]; // Buffer for old path
    char newpath[600]; // Buffer for new path

    // Remove the oldest rotated log file
    // snprintf -> Format string into buffer
    snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, LOG_MAX_ROTATIONS); // Oldest log file 
    unlink(oldpath); // Remove file

    // Shift rotated logs: N-1 → N, ..., 1 → 2
    // LOG_MAX_ROTATIONS - 1 down to 1
    for (int i = LOG_MAX_ROTATIONS - 1; i >= 1; i--) { // From N-1 down to 1

        snprintf(oldpath, sizeof(oldpath), "%s.%d", g_log_path, i); // Current log file
        snprintf(newpath, sizeof(newpath), "%s.%d", g_log_path, i + 1); // Next log file
        rename(oldpath, newpath); // Rename (move) file
    }

    // Move main log file to .1
    snprintf(newpath, sizeof(newpath), "%s.1", g_log_path); // New name for main log file
    rename(g_log_path, newpath); // Rename main log file

    // Reopen main log file (empty)
    // O_CREAT: create if not exists, O_WRONLY: write only, O_APPEND: append mode
    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644); // Open new log file
}


// #########################################################################################################
// Logger initialization
// #########################################################################################################

// Initializes the logger with the specified log file path.
void logger_init(const char* logfile_path) {
    // Store log file path
    // strncpy -> Safe string copy
    strncpy(g_log_path, logfile_path, sizeof(g_log_path) - 1); // Copy path

    g_log_path[sizeof(g_log_path) - 1] = '\0'; // Ensure null-termination

    // Create/open POSIX semaphore shared between processes
    g_sem = sem_open(LOG_SEM_NAME, O_CREAT, 0666, 1); // Initial value 1

    // Check for errors
    if (g_sem == SEM_FAILED) {
        perror("logger: sem_open");
        exit(1);
    }

    // Open log file
    g_log_fd = open(g_log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);

    // Check for errors
    if (g_log_fd < 0) {
        perror("logger: open logfile");
        exit(1);
    }
}


// #########################################################################################################
// Logger shutdown
// #########################################################################################################
void logger_close() { 
    // Close log file descriptor
    if (g_log_fd >= 0) { 
        close(g_log_fd); // Close file
        g_log_fd = -1; // Invalidate descriptor
    }


    // Close semaphore
    if (g_sem) {
        sem_close(g_sem);
        // NOTE: Do not call sem_unlink here to avoid race conditions on shutdown
    }
}

// #########################################################################################################
// Write a log entry (with semaphore + automatic rotation)
// #########################################################################################################

// Writes a log entry with the specified parameters.
void logger_write(
    const char* ip, // Client IP address
    const char* method, // HTTP method
    const char* path, // Request path
    int status, // HTTP status code
    size_t bytes_sent, // Number of bytes sent
    long duration_ms // Request duration in milliseconds
) {

    // Validate input parameters
    if (!g_sem || g_log_fd < 0) {
        return;
    }

    // Enter critical section (locks processes + threads)
    sem_wait(g_sem); 

    // Check if log file exceeds max size → rotate if needed
    // get_file_size -> Get current file size
    off_t size_now = get_file_size(g_log_fd); // Current log file size

    // Rotate logs if size exceeds limit
    if (size_now >= LOG_MAX_SIZE) {
        rotate_logs(); // Perform log rotation
    }

    // Get formatted timestamp
    char datebuf[64]; // Buffer for date string

    time_t now = time(NULL); // Current time

    struct tm tm_now; // Local time structure

    localtime_r(&now, &tm_now); // Convert to local time

    strftime(datebuf, sizeof(datebuf), "%d/%b/%Y:%H:%M:%S", &tm_now); // Format date string

    // Format log line
    char line[1024]; // Buffer for log line

    // snprintf -> Format string into buffer
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

    // Write log line atomically
    if (len > 0) { 
        // Write all bytes to log file
        if (write_all(g_log_fd, line, (size_t)len) != 0) { // Check for errors
            int e = errno; // Save errno
            dprintf(STDERR_FILENO, "logger: write failed: %s\n", strerror(e)); // Log error to stderr
        }    }

    // Leave critical section
    sem_post(g_sem);
}
