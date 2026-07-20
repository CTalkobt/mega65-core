CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2
LDFLAGS ?=

# Windows needs winsock
ifeq ($(OS),Windows_NT)
	LDFLAGS += -lws2_32
endif

SRCS = etherdbg.c commands.c protocol.c transport_udp.c
OBJS = $(SRCS:.c=.o)
TARGET = etherdbg

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Header dependencies
etherdbg.o: etherdbg.c transport.h transport_udp.h commands.h
commands.o: commands.c commands.h protocol.h transport.h
protocol.o: protocol.c protocol.h
transport_udp.o: transport_udp.c transport.h transport_udp.h

clean:
	rm -f $(OBJS) $(TARGET)
