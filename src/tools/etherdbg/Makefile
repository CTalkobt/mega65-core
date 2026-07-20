CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -std=c++23 -O2
LDFLAGS ?=

# Windows needs winsock
ifeq ($(OS),Windows_NT)
	LDFLAGS += -lws2_32
endif

SRCS = etherdbg.cpp commands.cpp protocol.cpp transport_udp.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = etherdbg

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies
etherdbg.o: etherdbg.cpp transport.h transport_udp.h commands.h
commands.o: commands.cpp commands.h protocol.h transport.h
protocol.o: protocol.cpp protocol.h
transport_udp.o: transport_udp.cpp transport.h transport_udp.h

clean:
	rm -f $(OBJS) $(TARGET)
