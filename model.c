#include "model.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MESSAGE_PERIOD 2
#define MESSAGE_LOSS_PERCENT 15
#define MESSAGE_DUPLICATE_PERCENT 15
#define MAX_MESSAGE_DELAY 3

static double min_double(double a, double b) {
    return a < b ? a : b;
}

static double abs_double(double x) {
    return x < 0.0 ? -x : x;
}

static int random_percent(void) {
    return rand() % 100;
}

static int random_delay(void) {
    return rand() % (MAX_MESSAGE_DELAY + 1);
}

// поиск места для нового сообщения
static Message *find_free_message_slot(WaterSystem *system) {
    for (int i = 0; i < MESSAGE_QUEUE_CAPACITY; ++i) {
        if (!system->messages[i].active) {
            return &system->messages[i];
        }
    }
    return NULL;
}

// вставка сообщения в очередь
static bool add_message_to_queue(WaterSystem *system, int id, int reservoir_id, int measured_tick, int delivery_tick, double measured_level) {
    Message *slot = find_free_message_slot(system);
    if (slot == NULL) {
        printf("[FATAL] Sensor message queue overflow!\n");
        system->fatal_accident = true;
        return false;
    }

    slot->active = true;
    slot->id = id;
    slot->reservoir_id = reservoir_id;
    slot->measured_tick = measured_tick;
    slot->delivery_tick = delivery_tick;
    slot->measured_level = measured_level;
    return true;
}

void ModelInit(WaterSystem *system) {
    memset(system, 0, sizeof(*system));

    system->reservoirs[0] = (Reservoir){
        .name = "North Reservoir",
        .capacity = 120.0,
        .level = 82.0,
        .previous_level = 82.0,
        .inflow = 6.0,
        .lower_bound = 28.0,
        .higher_bound = 105.0,
    };

    system->reservoirs[1] = (Reservoir){
        .name = "South Reservoir",
        .capacity = 100.0,
        .level = 64.0,
        .previous_level = 64.0,
        .inflow = 4.0,
        .lower_bound = 24.0,
        .higher_bound = 88.0,
    };

    system->districts[0] = (District){.name = "Central", .base_demand = 9.0};
    system->districts[1] = (District){.name = "East", .base_demand = 8.0};
    system->districts[2] = (District){.name = "West", .base_demand = 7.0};

    system->pumps[0] = (Pump){
        .name = "P1 (North -> Central)",
        .reservoir_id = 0,
        .district_id = 0,
        .capacity = 10.0,
        .power = 4.0,
    };
    system->pumps[1] = (Pump){
        .name = "P2 (North -> East)",
        .reservoir_id = 0,
        .district_id = 1,
        .capacity = 8.0,
        .power = 3.0,
    };
    system->pumps[2] = (Pump){
        .name = "P3 (South -> Central)",
        .reservoir_id = 1,
        .district_id = 0,
        .capacity = 8.0,
        .power = 3.0,
    };
    system->pumps[3] = (Pump){
        .name = "P4 (South -> West)",
        .reservoir_id = 1,
        .district_id = 2,
        .capacity = 9.0,
        .power = 4.0,
    };

    system->pipes[0] = (Pipe){.name = "T1", .capacity = 9.0};
    system->pipes[1] = (Pipe){.name = "T2", .capacity = 7.0};
    system->pipes[2] = (Pipe){.name = "T3", .capacity = 7.0};
    system->pipes[3] = (Pipe){.name = "T4", .capacity = 8.0};

    system->power_limit = 11.0;
    system->next_message_id = 1;
    system->fatal_accident = false;
}

void PrepareTick(WaterSystem *system, int tick) {
    // массив смещений для изменения потребности района
    static const double demand_offset[8] = {0.0, 2.0, 4.0, 1.0, -1.0, 3.0, 5.0, 1.0};

    for (int i = 0; i < DISTRICT_COUNT; ++i) {
        int phase = (tick + i * 2) % 8;
        system->districts[i].demand = system->districts[i].base_demand + demand_offset[phase];
        system->districts[i].received = 0.0;
        system->districts[i].deficit = 0.0;
    }
}

double ActivePumpPower(const WaterSystem *system) {
    double total = 0.0;
    for (int i = 0; i < PUMP_COUNT; ++i) {
        if (system->pumps[i].enabled) {
            total += system->pumps[i].power;
        }
    }
    return total;
}

void PhysicalStep(WaterSystem *system, int tick) {
    for (int r = 0; r < RESERVOIR_COUNT; ++r) {
        Reservoir *reservoir = &system->reservoirs[r];
        reservoir->previous_level = reservoir->level;
        reservoir->level += reservoir->inflow;
    }

    if (ActivePumpPower(system) > system->power_limit + 1e-9) {
        printf("[FATAL][Tick #%d] Active pump power exceeded the %.1f limit!\n", tick, system->power_limit);
        system->fatal_accident = true;
        return;
    }

    for (int p = 0; p < PUMP_COUNT; ++p) {
        Pump *pump = &system->pumps[p];
        const Pipe *pipe = &system->pipes[p];
        Reservoir *source = &system->reservoirs[pump->reservoir_id];
        District *district = &system->districts[pump->district_id];

        pump->previous_flow = pump->flow;
        pump->flow = 0.0;

        if (pump->enabled) {
            double possible = min_double(pump->capacity, pipe->capacity);
            possible = min_double(possible, source->level);

            double remaining_demand = district->demand - district->received;

            if (remaining_demand < 0.0) {
                remaining_demand = 0.0;
            }

            possible = min_double(possible, remaining_demand);

            pump->flow = possible;
            source->level -= possible;
            district->received += possible;
        }

        if (abs_double(pump->flow - pump->previous_flow) > 1e-9) {
            printf("[FLOW][Tick #%d] %s: %.1f -> %.1f units/tick\n", tick, pump->name, pump->previous_flow, pump->flow);
        }
    }

    for (int d = 0; d < DISTRICT_COUNT; ++d) {
        District *district = &system->districts[d];
        if (district->received + 1e-9 < district->demand) {
            district->deficit = district->demand - district->received;
            printf("[DEFICIT][Tick #%d] %s district faced deficit: %.1f (Demand: %.1f, Received: %.1f)!\n", tick, district->name, district->deficit, district->demand, district->received);
        }
    }

    for (int r = 0; r < RESERVOIR_COUNT; ++r) {
        Reservoir *reservoir = &system->reservoirs[r];

        if (reservoir->previous_level > reservoir->lower_bound && reservoir->level <= reservoir->lower_bound) {
            printf("[BOUND][Tick #%d] %s reached LOW level (%.1f)!\n", tick, reservoir->name, reservoir->level);
        }
        if (reservoir->previous_level < reservoir->higher_bound && reservoir->level >= reservoir->higher_bound) {
            printf("[BOUND][Tick #%d] %s reached HIGH level (%.1f)!\n", tick, reservoir->name, reservoir->level);
        }

        if (reservoir->level < -1e-9 || reservoir->level > reservoir->capacity + 1e-9) {
            printf("[FATAL][Tick #%d] %s is overflowed! Actual level (%.1f) is out of physical range [0, %.1f]\n", tick, reservoir->name, reservoir->level, reservoir->capacity);
            system->fatal_accident = true;
            return;
        }
    }
}

void GenerateMessages(WaterSystem *system, int tick) {
    if (tick % MESSAGE_PERIOD != 0) {
        return;
    }

    for (int r = 0; r < RESERVOIR_COUNT; ++r) {
        Reservoir *reservoir = &system->reservoirs[r];
        int id = system->next_message_id++;
        double measured = reservoir->level;

        printf("[SENSOR][Tick #%d] %s's level is %.1f (message #%d)\n", tick, reservoir->name, measured, id);

        if (random_percent() < MESSAGE_LOSS_PERCENT) {
            printf("[MESSAGE][Tick #%d] Message #%d was LOST!\n", tick, id);
            continue;
        }

        int delay = random_delay();
        if (!add_message_to_queue(system, id, r, tick, tick + delay, measured)) {
            return;
        }
        printf("[MESSAGE][Tick #%d] Message #%d is sent. Delivery tick is %d\n", tick, id, tick + delay);

        if (random_percent() < MESSAGE_DUPLICATE_PERCENT) {
            int duplicate_delay = delay + random_delay();
            if (!add_message_to_queue(system, id, r, tick, tick + duplicate_delay, measured)) {
                return;
            }
            printf("[MESSAGE][Tick #%d] Duplicate of message #%d is sent. Delivery tick is %d\n", tick, id, tick + duplicate_delay);
        }
    }
}
