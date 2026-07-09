#include "drd_manager.h"
#include "config.h"
#include <esp_timer.h>

#define DRD_MAGIC 0xD12DE5E7

RTC_NOINIT_ATTR uint32_t rtcDrdFlag;

static esp_timer_handle_t s_disarmTimer = nullptr;

static void disarmTimerCallback(void *arg) {
    rtcDrdFlag = 0;
}

bool drdCheckAndArm() {
    bool doubleReset = (rtcDrdFlag == DRD_MAGIC);

    if (doubleReset) {
        Serial.println("[DRD] Double reset detected -> manual recovery mode");
        rtcDrdFlag = 0;  
        return true;
    }

    rtcDrdFlag = DRD_MAGIC;

    esp_timer_create_args_t args = {};
    args.callback = &disarmTimerCallback;
    args.name = "drd_disarm";
    if (esp_timer_create(&args, &s_disarmTimer) == ESP_OK) {
        esp_timer_start_once(s_disarmTimer, (uint64_t)DRD_WINDOW_MS * 1000ULL);
    }

    return false;
}

void drdDisarm() {
    rtcDrdFlag = 0;
    if (s_disarmTimer) {
        esp_timer_stop(s_disarmTimer);
    }
}
