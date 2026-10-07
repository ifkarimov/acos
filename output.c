#include "output.h"

#include <stdio.h>

void PrintStatus(const WaterSystem *system, const Dispatcher *dispatcher, int tick) {
    printf("\n\n========== TICK %d: CURRENT STATUS and DISPATCHER VIEW ==========\n", tick);

    printf("Reservoirs:\n");
    for (int r = 0; r < RESERVOIR_COUNT; ++r) {
        const Reservoir *reservoir = &system->reservoirs[r];

        if (!dispatcher->has_level[r]) {
            printf("  %s: actual = %.1f/%.1f, dispatcher = UNKNOWN\n", reservoir->name, reservoir->level, reservoir->capacity);
        } else if (dispatcher->known_message_id[r] == 0) {
            printf("  %s: actual = %.1f/%.1f, dispatcher = %.1f (initial data)\n", reservoir->name, reservoir->level, reservoir->capacity, dispatcher->known_level[r]);
        } else {
            printf("  %s: actual = %.1f/%.1f, dispatcher = %.1f (measurement: tick %d, message #%d)\n", reservoir->name, reservoir->level, reservoir->capacity, dispatcher->known_level[r], dispatcher->known_measure_tick[r], dispatcher->known_message_id[r]);
        }
    }

    printf("Districts:\n");
    for (int d = 0; d < DISTRICT_COUNT; ++d) {
        const District *district = &system->districts[d];
        printf("  %s: demand = %.1f, received = %.1f, deficit = %.1f\n", district->name, district->demand, district->received, district->deficit);
    }

    printf("Pumps:\n");
    for (int p = 0; p < PUMP_COUNT; ++p) {
        const Pump *pump = &system->pumps[p];
        printf("  %s is %s: flow = %.1f, power = %.1f\n", pump->name, pump->enabled ? "ON" : "OFF", pump->flow, pump->enabled ? pump->power : 0.0);
    }

    printf("Total active power: %.1f/%.1f\n", ActivePumpPower(system), system->power_limit);

    printf("============================ TICK %d ============================\n\n\n", tick);
}
