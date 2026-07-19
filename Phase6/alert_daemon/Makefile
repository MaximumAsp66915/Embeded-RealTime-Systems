CXX      = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra
LIBS     = -lsqlite3
INCLUDES = -Isrc

TARGET   = alert_daemon
SRCS     = src/main.cpp
OBJS     = $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
