# Makefile — Smart Surveillance System, Step 5 (MQTT + Email)
#
# Build:   make
# Run:     ./surveillance_notifier notifier.conf
# Clean:   make clean
#
# Requires: gcc, libmosquitto-dev, libcurl4-openssl-dev (or any libcurl4-*-dev), pthread
#   Debian/Ubuntu: sudo apt install build-essential libmosquitto-dev libcurl4-openssl-dev

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -Isrc
LDFLAGS = -lmosquitto -lcurl -lpthread

SRC_DIR = src
BUILD_DIR = build
TARGET = surveillance_notifier

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
	./$(TARGET) notifier.conf
