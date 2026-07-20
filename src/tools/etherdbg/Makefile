CXX ?= g++
CXXFLAGS ?= -Wall -Wextra -std=c++23 -O2
LDFLAGS ?=

# Windows needs winsock
ifeq ($(OS),Windows_NT)
	LDFLAGS += -lws2_32
endif

# Optional PNG support: compile with `make HAS_PNG=1`
ifdef HAS_PNG
	CXXFLAGS += -DHAS_LIBPNG
	LDFLAGS += -lpng
endif

SRCS = etherdbg.cpp commands.cpp protocol.cpp transport_udp.cpp screen.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = etherdbg

TEST_SRCS = test_etherdbg.cpp commands.cpp protocol.cpp
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TEST_TARGET = test_etherdbg

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Header dependencies
etherdbg.o: etherdbg.cpp transport.h transport_udp.h commands.h screen.h
commands.o: commands.cpp commands.h protocol.h transport.h
protocol.o: protocol.cpp protocol.h
transport_udp.o: transport_udp.cpp transport.h transport_udp.h
screen.o: screen.cpp screen.h commands.h transport.h
test_etherdbg.o: test_etherdbg.cpp protocol.h transport.h commands.h

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET) $(TEST_TARGET)
