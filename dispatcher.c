#include "dispatcher.h"

#include <string.h>
#include <stdio.h>

static bool message_was_processed(const Dispatcher *dispatcher, int id) {
    int count = dispatcher->processed_count;
    if (count > PROCESSED_MESSAGE_HISTORY) {
        count = PROCESSED_MESSAGE_HISTORY;
    }
    for (int i = 0; i < count; ++i) {
        if (dispatcher->processed_ids[i] == id) {
            return true;
        }
    }
    return false;
}

static void remember_message(Dispatcher *dispatcher, int id) {
    dispatcher->processed_ids[dispatcher->processed_next] = id;
    dispatcher->processed_next = (dispatcher->processed_next + 1) % PROCESSED_MESSAGE_HISTORY;
    if (dispatcher->processed_count < PROCESSED_MESSAGE_HISTORY) {
        ++dispatcher->processed_count;
    }
}

void DispatcherInit(Dispatcher *dispatcher, const WaterSystem *system) {
    memset(dispatcher, 0, sizeof(*dispatcher));

    // допускаем, что при запуске диспетчер знает начальные уровни
    for (int r = 0; r < RESERVOIR_COUNT; ++r) {
        dispatcher->has_level[r] = true;
        dispatcher->known_level[r] = system->reservoirs[r].level;
        dispatcher->known_measure_tick[r] = 0;
        dispatcher->known_message_id[r] = 0;
    }
}

void DispatcherReceiveMessages(WaterSystem *system, Dispatcher *dispatcher, int tick) {
    for (int i = 0; i < MESSAGE_QUEUE_CAPACITY; ++i) {
        Message *message = &system->messages[i];
        if (!message->active || message->delivery_tick > tick) {
            continue;
        }

        ++system->received_count;
        printf("[MESSAGE][Tick #%d] Dispatcher received message #%d from %s: the level is %.1f (measured at tick #%d)\n", tick, message->id, system->reservoirs[message->reservoir_id].name, message->measured_level, message->measured_tick);

        if (message_was_processed(dispatcher, message->id)) {
            ++system->ignored_duplicate_count;
            printf("[MESSAGE][Tick #%d] Message #%d is a duplicate. It is ignored\n", tick, message->id);
            message->active = false;
            continue;
        }

        remember_message(dispatcher, message->id);
        int r = message->reservoir_id;
        dispatcher->has_level[r] = true;

        if (message->measured_tick > dispatcher->known_measure_tick[r] ||
            (message->measured_tick == dispatcher->known_measure_tick[r] && message->id > dispatcher->known_message_id[r])) {
            dispatcher->known_level[r] = message->measured_level;
            dispatcher->known_measure_tick[r] = message->measured_tick;
            dispatcher->known_message_id[r] = message->id;
        } else {
            ++system->ignored_old_count;
            printf("[MESSAGE][Tick #%d] Message #%d contains old data. It is ignored\n", tick, message->id);    
        }

        message->active = false;
    }
}

static bool reservoir_is_available(const WaterSystem *system, const Dispatcher *dispatcher, int reservoir_id) {
    if (!dispatcher->has_level[reservoir_id]) {
        return false;
    }
    return dispatcher->known_level[reservoir_id] > system->reservoirs[reservoir_id].lower_bound;
}

static bool can_enable(const WaterSystem *system, const Dispatcher *dispatcher, int pump_id, double used_power) {
    const Pump *pump = &system->pumps[pump_id];
    return reservoir_is_available(system, dispatcher, pump->reservoir_id) && used_power + pump->power <= system->power_limit + 1e-9;
}

void DispatcherControl(WaterSystem *system, const Dispatcher *dispatcher, int tick) {
    bool desired[PUMP_COUNT] = {false};
    double used_power = 0.0;

    double central_demand = system->districts[0].demand;
    double central_supply = 0.0;

    if (can_enable(system, dispatcher, 0, used_power)) {
        desired[0] = true;
        used_power += system->pumps[0].power;

        central_supply = system->pumps[0].capacity;

        if (system->pipes[0].capacity < central_supply) {
            central_supply = system->pipes[0].capacity;
        }
    }

    if (central_supply < central_demand && can_enable(system, dispatcher, 2, used_power)) {
        desired[2] = true;
        used_power += system->pumps[2].power;
    }

    if (can_enable(system, dispatcher, 1, used_power)) {
        desired[1] = true;
        used_power += system->pumps[1].power;
    }

    if (can_enable(system, dispatcher, 3, used_power)) {
        desired[3] = true;
        used_power += system->pumps[3].power;
    }

    bool changed = false;

    // сначала освобождаем мощность, потом включаем остальные насосы
    for (int p = 0; p < PUMP_COUNT; ++p) {
        if (system->pumps[p].enabled && !desired[p]) {
            system->pumps[p].enabled = false;
            changed = true;
            printf("[CONTROL][Tick #%d] Dispatcher turns %s OFF\n", tick, system->pumps[p].name);
        }
    }

    for (int p = 0; p < PUMP_COUNT; ++p) {
        if (!system->pumps[p].enabled && desired[p]) {
            system->pumps[p].enabled = true;
            changed = true;
            printf("[CONTROL][Tick #%d] Dispatcher turns %s ON\n", tick, system->pumps[p].name);
        }
    }

    if (changed) {
        printf("[CONTROL][Tick #%d] Active pump power is %.1f/%.1f\n", tick, used_power, system->power_limit);
    }
}
