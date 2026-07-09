#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>
#include <time.h>

bool wifiConnect() {
    const char *TEST_SSID = "Himaa";
    const char *TEST_PASS = "Hima_2002";

    WiFi.mode(WIFI_STA);
    WiFi.begin(TEST_SSID, TEST_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
            Serial.println("[WiFi] connect timeout - falling back");
            return false;
        }
        delay(WIFI_RETRY_DELAY_MS);
    }

    Serial.print("[WiFi] connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
}

bool wifiIsConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifiSyncTime() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

    struct tm timeinfo;
    uint32_t start = millis();
    while (!getLocalTime(&timeinfo, 100)) {
        if (millis() - start > 10000) {
            Serial.println("[WiFi] NTP sync timeout, continuing without synced clock");
            return;
        }
    }
    Serial.println("[WiFi] time synced");
}
