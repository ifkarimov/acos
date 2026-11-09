#include "dispatcher.h"
#include "model.h"

#include <assert.h>
#include <stdio.h>

static double abs_double(double value) {
    return value < 0.0 ? -value : value;
}

static void test_pumps(void) {
    WaterSystem system;
    ModelInit(&system);

    system.pumps[0].enabled = true;
    system.pumps[2].enabled = true;
    system.districts[0].demand = 12.0;
    double before = system.reservoirs[0].level + system.reservoirs[1].level;

    PumpStep(&system, 0, 0);
    PumpStep(&system, 2, 0);
    double after = system.reservoirs[0].level + system.reservoirs[1].level;

    assert(abs_double(system.districts[0].received - 12.0) < 1e-9);
    assert(abs_double(before - after - 12.0) < 1e-9);
    assert(system.pumps[0].flow <= system.pipes[0].capacity);
    assert(system.pumps[2].flow <= system.pipes[2].capacity);

    ModelInit(&system);
    system.pumps[0].enabled = true;
    system.reservoirs[0].level = 2.0;
    system.districts[0].demand = 20.0;
    PumpStep(&system, 0, 0);
    assert(system.pumps[0].flow == 2.0);
    assert(system.reservoirs[0].level == 0.0);
    assert(!system.fatal_accident);
}

static void test_control(void) {
    WaterSystem system;
    Dispatcher dispatcher;
    ModelInit(&system);
    DispatcherInit(&dispatcher, &system);
    system.districts[0].demand = 13.0;
    DispatcherControl(&system, &dispatcher, 0);
    assert(system.pumps[0].enabled);
    assert(system.pumps[2].enabled);
    assert(ActivePumpPower(&system) <= system.power_limit);

    // физический уровень изменился, но диспетчер ещё не получил сообщение
    system.reservoirs[0].level = 0.0;
    DispatcherControl(&system, &dispatcher, 1);
    assert(system.pumps[0].enabled);

    dispatcher.known_level[0] = 0.0;
    DispatcherControl(&system, &dispatcher, 2);
    assert(!system.pumps[0].enabled);
    assert(!system.pumps[1].enabled);

    system.power_limit = 0.0;
    DispatcherControl(&system, &dispatcher, 3);
    assert(ActivePumpPower(&system) == 0.0);
}

static void test_messages(void) {
    WaterSystem system;
    Dispatcher dispatcher;
    ModelInit(&system);
    DispatcherInit(&dispatcher, &system);

    system.messages[0] = (Message){true, 1, 0, 1, 5, 40.0};
    system.messages[1] = (Message){true, 2, 0, 2, 2, 50.0};
    system.messages[2] = (Message){true, 2, 0, 2, 3, 50.0};
    DispatcherReceiveMessages(&system, &dispatcher, 2);
    assert(dispatcher.known_level[0] == 50.0);
    assert(system.messages[0].active);

    DispatcherReceiveMessages(&system, &dispatcher, 3);
    assert(system.ignored_duplicate_count == 1);
    DispatcherReceiveMessages(&system, &dispatcher, 5);
    assert(system.ignored_old_count == 1);
    assert(dispatcher.known_level[0] == 50.0);

    // два измерения за один такт: более раннее не должно заменять более позднее
    system.messages[0] = (Message){true, 4, 0, 6, 6, 60.0};
    system.messages[1] = (Message){true, 3, 0, 6, 7, 55.0};
    DispatcherReceiveMessages(&system, &dispatcher, 7);
    assert(dispatcher.known_message_id[0] == 4);
    assert(dispatcher.known_level[0] == 60.0);
}

static void test_accidents(void) {
    WaterSystem system;
    ModelInit(&system);
    system.reservoirs[0].level = system.reservoirs[0].capacity;
    ReservoirStep(&system, 0, 0);
    assert(system.fatal_accident);

    ModelInit(&system);
    for (int p = 0; p < PUMP_COUNT; ++p) {
        system.pumps[p].enabled = true;
    }
    PumpStep(&system, 0, 0);
    assert(system.fatal_accident);

    // заполненная очередь должна дать аварию, а не выход за границу массива
    ModelInit(&system);
    for (int i = 0; i < MESSAGE_QUEUE_CAPACITY; ++i) {
        system.messages[i].active = true;
    }
    unsigned int seed = 42;
    for (int i = 0; i < 100 && !system.fatal_accident; ++i) {
        GenerateMessage(&system, 0, 0, &seed);
    }
    assert(system.fatal_accident);
}

static void test_statistics(void) {
    WaterSystem system;
    ModelInit(&system);
    DistrictPrepare(&system, 0, 0);
    system.districts[0].received = 5.0;
    DistrictFinish(&system, 0, 0);
    assert(system.districts[0].total_demand == 9.0);
    assert(system.districts[0].total_received == 5.0);
    assert(system.districts[0].total_deficit == 4.0);
    DistrictPrepare(&system, 0, 1);
    assert(system.districts[0].received == 0.0);
    assert(system.districts[0].total_received == 5.0);
}

int main(void) {
    test_pumps();
    test_control();
    test_messages();
    test_accidents();
    test_statistics();
    puts("Model tests passed");
    return 0;
}
