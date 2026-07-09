#include "scheduler.h"
#include "config.h"
#include "update_trigger.h"
#include <time.h>

static uint32_t s_lastIntervalCheckMs = 0;
static uint32_t s_lastPollCheckMs = 0;
static int s_lastDailyTriggerYday = -1;   // the midnight check
static uint32_t s_lastTickSecondMs = 0;

static uint32_t s_buttonPressStart = 0;
static volatile bool s_buttonFlag = false;

void IRAM_ATTR buttonISR() {
    s_buttonFlag = true;
}

void schedulerInit() {
    pinMode(BUTTON_PIN, INPUT_PULLUP);  
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
}

static void checkDailySchedule() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) {
        return;  
    }

    if (timeinfo.tm_hour == DAILY_CHECK_HOUR &&
        timeinfo.tm_min == DAILY_CHECK_MINUTE &&
        timeinfo.tm_yday != s_lastDailyTriggerYday) {
        s_lastDailyTriggerYday = timeinfo.tm_yday;
        Serial.println("[Scheduler] daily 12:00 AM trigger");
        raiseUpdateTrigger(UpdateReason::SCHEDULE_DAILY);
    }
}

static void checkIntervalSchedule() {
    uint32_t now = millis();
    if (now - s_lastIntervalCheckMs >= INTERVAL_CHECK_MS) {
        s_lastIntervalCheckMs = now;
        Serial.println("[Scheduler] 3-hour interval trigger");
        raiseUpdateTrigger(UpdateReason::SCHEDULE_INTERVAL);
    }
}

static void checkPollSchedule() {
    uint32_t now = millis();
    if (now - s_lastPollCheckMs >= POLL_INTERVAL_MS) {
        s_lastPollCheckMs = now;
        // No log line here on purpose - this fires every 20s and would
        // flood the monitor. otaCheckAndUpdate() already logs when it
        // actually finds something new.
        raiseUpdateTrigger(UpdateReason::SCHEDULE_POLL);
    }
}

static void checkButton() {
    if (!s_buttonFlag) return;
    s_buttonFlag = false;

    delay(BUTTON_DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) == LOW) {
        Serial.println("[Scheduler] push-button trigger (manual check-now)");
        raiseUpdateTrigger(UpdateReason::BUTTON_PRESS);
    }
}

void schedulerTick() {
    uint32_t now = millis();
    if (now - s_lastTickSecondMs >= 1000) {
        s_lastTickSecondMs = now;
        checkDailySchedule();
        checkIntervalSchedule();
        checkPollSchedule();
    }
    checkButton();
}
