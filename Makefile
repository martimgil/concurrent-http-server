# Makefile for Concurrent HTTP Web Server

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -O2 -MMD -MP $(CFLAGS_EXTRA)
LDFLAGS = -pthread -lrt $(LDFLAGS_EXTRA)

# Directories
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

# Target executable
TARGET = $(BIN_DIR)/webserver

# Source files
SOURCES = $(SRC_DIR)/master.c \
          $(SRC_DIR)/worker.c \
          $(SRC_DIR)/shared_mem.c \
          $(SRC_DIR)/semaphores.c \
          $(SRC_DIR)/thread_pool.c \
          $(SRC_DIR)/http_builder.c \
          $(SRC_DIR)/http_parser.c \
          $(SRC_DIR)/config.c \
          $(SRC_DIR)/cache.c \
          $(SRC_DIR)/logger.c \
          $(SRC_DIR)/thread_logger.c \
          $(SRC_DIR)/stats.c

# Object & dep files
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS    = $(OBJECTS:.o=.d)

# Default target
all: directories $(TARGET)

# Create necessary directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

# Link object files to create executable
$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "Build complete: $@"

# Compile source files to object files (deps auto-geradas por -MMD -MP)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Include auto-generated dependency files
-include $(DEPS)

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	@echo "Clean complete."

# Run the server
run: $(TARGET)
	@echo "Starting web server..."
	./$(TARGET)

# Debug build
debug: CFLAGS += -DDEBUG -g3
debug: clean all

# Install dependencies (if needed)
install-deps:
	@echo "No external dependencies required."

# Run tests
test: $(TARGET)
	@echo "Running test suite..."
	./tests/test_suite.sh

# Build tests
build-tests: $(TARGET)
	@echo "Building test binaries..."
	gcc -Wall -Wextra -pthread -g -O2 -Isrc tests/test_concurrent.c build/cache.o -o tests/test_cache_consistency

# Display help
help:
	@echo "Available targets:"
	@echo "  all                 - Build the project (default)"
	@echo "  clean               - Remove all build artifacts"
	@echo "  run                 - Build and run the server"
	@echo "  debug               - Build with debug symbols"
	@echo "  test                - Run the test suite"
	@echo "  build-tests         - Build test binaries"
	@echo "  install-deps        - Install required dependencies"
	@echo "  help                - Display this help message"
	@echo ""
	@echo "Automated race detection (no manual make needed):"
	@echo "  ./tests/test_suite.sh --race-helgrind   - Run tests with Helgrind"
	@echo "  ./tests/test_suite.sh --race-tsan       - Run tests with Thread Sanitizer"
	@echo "  ./tests/test_suite.sh                   - Run tests normally"

# Phony targets
.PHONY: all clean run debug install-deps help directories test build-tests
