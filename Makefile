CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2
IPC_FLAGS = -pthread -lrt
SOURCES = main.c model.c dispatcher.c output.c simulation.c ipc.c
HEADERS = model.h dispatcher.h output.h simulation.h ipc.h

.PHONY: all clean test

all: water_system water_system_unnamed water_independent

water_system: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(IPC_FLAGS) -o $@

water_system_unnamed: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DUSE_UNNAMED_SEMAPHORES $(SOURCES) $(IPC_FLAGS) -o $@

water_independent: independent.c model.c dispatcher.c output.c model.h dispatcher.h output.h
	$(CC) $(CPPFLAGS) $(CFLAGS) independent.c model.c dispatcher.c output.c $(IPC_FLAGS) -o $@

test: all
	python3 tests/run_tests.py
	python3 tests/run_tests.py water_system_unnamed
	python3 tests/process_tests.py
	python3 tests/independent_tests.py

clean:
	rm -f water_system water_system_unnamed water_independent
