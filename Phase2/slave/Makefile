# Slave Node Compiler Rules
CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
LIBS     = -lsqlite3 -lpthread -lmemcached
INCLUDES = -Isrc

TARGET   = slave_node
SRCS     = src/main.cpp src/mongoose.c
OBJS     = $(SRCS:.c=.o)
OBJS    := $(OBJS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	gcc -O3 -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)