CXX      = g++
CC       = gcc
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
CFLAGS   = -O3 -Wall -Wextra
LIBS     = -lsqlite3 -lpthread -lmemcached -lpaho-mqtt3c
INCLUDES = -Isrc

TARGET    = master_node
CXX_SRCS  = src/main.cpp src/api.cpp
C_SRCS    = src/mongoose.c
OBJS      = $(CXX_SRCS:.cpp=.o) $(C_SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
