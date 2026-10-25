CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2
THREAD_FLAGS = -pthread
SOURCES = main.c model.c dispatcher.c output.c simulation.c
HEADERS = model.h dispatcher.h output.h simulation.h

.PHONY: all clean test

all: water_system

water_system: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $(SOURCES) -o $@

test: water_system
	python3 tests/run_tests.py

clean:
	rm -f water_system
