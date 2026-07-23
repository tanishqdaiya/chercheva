CC = gcc
CFLAGS = -Wall -Wextra -pedantic -ggdb
CFLAGS += -Ilib

LDFLAGS = lib/liblexbor_static.a

TARGET = engine

SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
