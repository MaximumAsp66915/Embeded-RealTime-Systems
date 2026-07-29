# Makefile — Smart Surveillance System, Step 3 (Web Server)
#
# Build:   make
# Run:     ./surveillance_web server.conf
# Clean:   make clean
#
# Requires: gcc, libssl-dev (OpenSSL headers), pthread (part of glibc)
#   Debian/Ubuntu: sudo apt install build-essential libssl-dev

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -Isrc
LDFLAGS = -lssl -lcrypto -lpthread

SRC_DIR = src
BUILD_DIR = build
TARGET = surveillance_web

SOURCES = $(wildcard $(SRC_DIR)/*.c)
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

run: all
	sudo ./$(TARGET) server.conf
