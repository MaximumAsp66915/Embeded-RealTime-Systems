# Master Core Compiler Rules (Pure MQTT)
CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
LIBS     = -lsqlite3 -lpthread -lmemcached -lpaho-mqtt3c
INCLUDES = -Isrc

TARGET   = master_node
SRCS     = src/main.cpp
OBJS     = $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)