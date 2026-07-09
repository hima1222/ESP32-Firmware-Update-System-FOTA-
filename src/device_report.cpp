#include "device_report.h"
#include "config.h"
#include "ota_manager.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

extern String g_deviceId;

#define REPORT_INTERVAL_MS (30UL * 1000UL)  

static uint32_t s_lastReportMs = 0;

static void sendReport() {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();  

    HTTPClient http;
    String url = String(FOTA_BASE_URL) + FOTA_REPORT_PATH;
    if (!http.begin(client, url)) {
        Serial.println("[Report] http.begin() failed");
        return;
    }
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["deviceId"] = g_deviceId;
    doc["firmware"] = otaGetCurrentVersion();
    doc["ip"] = WiFi.localIP().toString();

    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code > 0) {
        Serial.printf("[Report] POST %s -> HTTP %d\n", url.c_str(), code);
    } else {
        Serial.printf("[Report] POST failed: %s\n", http.errorToString(code).c_str());
    }
    http.end();
}

void reportInit() {
    sendReport();
    s_lastReportMs = millis();
}

void reportTick() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (millis() - s_lastReportMs >= REPORT_INTERVAL_MS) {
        s_lastReportMs = millis();
        sendReport();
    }
}

void reportNow(const String &status) {
    (void)status;  
    sendReport();
    s_lastReportMs = millis();
}
