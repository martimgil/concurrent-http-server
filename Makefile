# Makefile for Concurrent HTTP Web Server

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -O2
LDFLAGS = -pthread -lrt

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

# Object files
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Header files
HEADERS = $(SRC_DIR)/shared_mem.h \
          $(SRC_DIR)/semaphores.h \
          $(SRC_DIR)/thread_pool.h \
          $(SRC_DIR)/config.h \
          $(SRC_DIR)/cache.h \
          $(SRC_DIR)/logger.h \
          $(SRC_DIR)/stats.h \
          $(SRC_DIR)/worker.h

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

# Compile source files to object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR) $(BIN_DIR)
	rm -f $(SRC_DIR)/*.o
	rm -f $(SRC_DIR)/master
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

# Display help
help:
	@echo "Available targets:"
	@echo "  all          - Build the project (default)"
	@echo "  clean        - Remove all build artifacts"
	@echo "  run          - Build and run the server"
	@echo "  debug        - Build with debug symbols"
	@echo "  install-deps - Install required dependencies"
	@echo "  help         - Display this help message"

# Phony targets
.PHONY: all clean run debug install-deps help directories

