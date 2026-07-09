#pragma once
#include <Arduino.h>

enum class UpdateReason {
    NONE,
    SCHEDULE_DAILY,
    SCHEDULE_INTERVAL,
    SCHEDULE_POLL,
    MQTT_EVENT,
    BUTTON_PRESS
};

extern volatile bool g_updatePending;
extern volatile UpdateReason g_updateReason;

void raiseUpdateTrigger(UpdateReason reason);

bool consumeUpdateTrigger(UpdateReason &reasonOut);
