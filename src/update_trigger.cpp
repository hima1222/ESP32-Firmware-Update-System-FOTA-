#include "update_trigger.h"

volatile bool g_updatePending = false;
volatile UpdateReason g_updateReason = UpdateReason::NONE;

void raiseUpdateTrigger(UpdateReason reason) {
    if (!g_updatePending) {
        g_updateReason = reason;
        g_updatePending = true;
    }
}

bool consumeUpdateTrigger(UpdateReason &reasonOut) {
    if (g_updatePending) {
        reasonOut = g_updateReason;
        g_updatePending = false;
        g_updateReason = UpdateReason::NONE;
        return true;
    }
    return false;
}
