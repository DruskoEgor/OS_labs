CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDLIBS ?= -pthread

all: lab2

lab2: lab2.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f lab2 result.txt small.txt timings*.csv

.PHONY: all clean
