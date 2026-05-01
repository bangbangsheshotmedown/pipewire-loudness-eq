CC = gcc
CFLAGS = -Wall -O3 $(shell pkg-config --cflags libpipewire-0.3)
LIBS = $(shell pkg-config --libs libpipewire-0.3) -lm

all: loudness-eq

loudness-eq: loudness.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f loudness-eq loudness-eq-claude

.PHONY: all clean
