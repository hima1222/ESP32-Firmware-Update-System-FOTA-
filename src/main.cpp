// FOTA firmware - ESP32 update client
// Update triggers (any one calls otaCheckAndUpdate()):
//   1. Every day at 12:00 AM        
//   2. Every 3 hours                
//   3. Event-driven via MQTT        
//   4. Push-button "check now"     
//
// Rollback:
//   - Automatic: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (set by default.csv's
//     ota_0/ota_1 partitioning) + esp_ota_mark_app_valid_cancel_rollback()
//     after a self-test, or esp_ota_mark_app_invalid_rollback_and_reboot()
//     if the self-test fails.
//   - Manual: Double Reset Detection (drd_manager.cpp) flips the boot
//     partition back to the other OTA slot, for when a device is physically
//     reachable and needs recovery regardless of self-test state.


#include <Arduino.h>
#include <esp_mac.h>

#include "config.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "scheduler.h"
#include "drd_manager.h"
#include "ota_manager.h"
#include "update_trigger.h"
#include "device_report.h"

String g_deviceId;  

static void buildDeviceId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[13];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_deviceId = String(buf);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[BOOT] FOTA firmware starting...");

    buildDeviceId();
    Serial.println("[BOOT] device id: " + g_deviceId);
    otaLogBootInfo();

    // DRD check 
    if (drdCheckAndArm()) {
        otaManualRollback();
    }

    // Self-test 
    otaRunSelfTestAndConfirm();

    //  WiFi 
    if (!wifiConnect()) {
        Serial.println("[BOOT] WiFi failed - will keep retrying update checks; "
                        "hook this into your captive-portal fallback if needed.");
    } else {
        wifiSyncTime();
        mqttManagerInit();
        reportInit();
    }

    schedulerInit();

    pinMode(LED_PIN, OUTPUT);

    Serial.println("[BOOT] setup complete, current firmware version: " + otaGetCurrentVersion());
}

void loop() {
    schedulerTick();
    reportTick();

    UpdateReason reason;
    if (consumeUpdateTrigger(reason)) {
        if (!wifiIsConnected()) {
            Serial.println("[LOOP] update trigger fired but WiFi is down, retrying connect...");
            wifiConnect();
        }

        if (wifiIsConnected()) {
            OtaResult result = otaCheckAndUpdate();
            switch (result) {
                case OtaResult::UP_TO_DATE:
                    mqttPublishStatus("up_to_date");
                    reportNow("up_to_date");
                    break;
                case OtaResult::HW_INCOMPATIBLE:
                    mqttPublishStatus("hw_incompatible");
                    reportNow("hw_incompatible");
                    break;
                case OtaResult::METADATA_FETCH_FAILED:
                    mqttPublishStatus("metadata_fetch_failed");
                    reportNow("metadata_fetch_failed");
                    break;
                case OtaResult::DOWNLOAD_FAILED:
                    mqttPublishStatus("download_or_verify_failed");
                    reportNow("download_or_verify_failed");
                    break;
                default:
                    break;
            }
        }
    }

    //  Test LED blink 
    
    static uint32_t lastBlink = 0;
    static bool ledState = false;
    if (millis() - lastBlink >= LED_BLINK_INTERVAL_MS) {
        lastBlink = millis();
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
    }

    // rest of the application logic 

    delay(20);
}
