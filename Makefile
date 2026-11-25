CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2
IPC_FLAGS = -pthread -lrt
SOURCES = main.c model.c dispatcher.c output.c simulation.c ipc.c
HEADERS = model.h dispatcher.h output.h simulation.h ipc.h
NETWORK_OBJECTS = network_model.o network_dispatcher.o network_status.o
NETWORK_PROGRAMS = water_server water_client water_observer water_admin

.PHONY: all clean test network test-network

all: water_system water_system_unnamed water_independent network

water_system: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(IPC_FLAGS) -o $@

water_system_unnamed: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DUSE_UNNAMED_SEMAPHORES $(SOURCES) $(IPC_FLAGS) -o $@

water_independent: independent.c model.c dispatcher.c output.c model.h dispatcher.h output.h
	$(CC) $(CPPFLAGS) $(CFLAGS) independent.c model.c dispatcher.c output.c $(IPC_FLAGS) -o $@

network: $(NETWORK_PROGRAMS)

network_model.o: model.c model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dprintf=NetworkPrint -c model.c -o $@

network_dispatcher.o: dispatcher.c dispatcher.h model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dprintf=NetworkPrint -c dispatcher.c -o $@

network_status.o: output.c output.h dispatcher.h model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dprintf=NetworkPrint -c output.c -o $@

water_server: server.c network.c network_output.c network.h $(NETWORK_OBJECTS)
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c network.c network_output.c $(NETWORK_OBJECTS) -o $@

water_client: client.c network_client.c network.c network.h model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) client.c network_client.c network.c -o $@

water_observer: observer.c network_client.c network.c network.h model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) observer.c network_client.c network.c -o $@

water_admin: admin.c network_client.c network.c network.h model.h
	$(CC) $(CPPFLAGS) $(CFLAGS) admin.c network_client.c network.c -o $@

test-network: network
	python3 tests/network_tests.py

test: all
	python3 tests/run_tests.py
	python3 tests/run_tests.py water_system_unnamed
	python3 tests/process_tests.py
	python3 tests/independent_tests.py
	python3 tests/network_tests.py

clean:
	rm -f water_system water_system_unnamed water_independent
	rm -f $(NETWORK_PROGRAMS) $(NETWORK_OBJECTS)
