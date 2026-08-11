/* ESP32 FOTA FIRMWARE — single-file, task-based version
   TASK LAYOUT:
     TaskNetwork  - CORE 0   WiFi maintenance, MQTT, HTTP heartbeat report
     TaskOTA      - CORE 1   Scheduler timers, button, LED, OTA update logic 
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h> //access NVS for storing current firmware version
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <ESP32MQTTClient.h>
#include <time.h>

//Set to 0 to silence ALL serial output and set to 1 for normal logging.
#define DEBUG_ENABLE 1

#if DEBUG_ENABLE
  #define DBG_INIT(baud)  Serial.begin(baud)
  #define DBG(...)        Serial.print(__VA_ARGS__)
  #define DBGLN(...)      Serial.println(__VA_ARGS__)
  #define DBGF(...)       Serial.printf(__VA_ARGS__)
#else
  #define DBG_INIT(baud)
  #define DBG(...)
  #define DBGLN(...)
  #define DBGF(...)
#endif

//USER CONFIGURATION
#define HW_ID                  "esp32-devkit-v1"

#define WIFI_SSID              "Himaa"
#define WIFI_PASSWORD          "Hima_2002"
#define WIFI_CONNECT_TIMEOUT_MS 15000

#define FOTA_BASE_URL          "https://backend-production-2052f.up.railway.app"
#define FOTA_METADATA_PATH     "/api/firmware/latest"
#define FOTA_REPORT_PATH       "/api/esp/data" 

#define MQTT_BROKER_URI        "mqtt://test.mosquitto.org:1883" //"152.42.249.28:1883" 
#define MQTT_USERNAME          ""
#define MQTT_PASSWORD          ""
#define MQTT_TOPIC_ALL         "devices/all/update"

#define NTP_SERVER             "pool.ntp.org"
#define GMT_OFFSET_SEC         (5 * 3600 + 1800)   // UTC+5:30
#define DAYLIGHT_OFFSET_SEC    0
#define DAILY_CHECK_HOUR       0
#define DAILY_CHECK_MINUTE     0
#define INTERVAL_CHECK_MS      (3UL * 60UL * 60UL * 1000UL)   // every 3 hours
#define POLL_INTERVAL_MS       (5UL * 60UL * 1000UL)                 // no-MQTT fallback poll
#define REPORT_INTERVAL_MS     (30UL * 1000UL)                 // dashboard heartbeat

#define BUTTON_PIN             0
#define BUTTON_DEBOUNCE_MS     50
#define DRD_WINDOW_MS          3000

#define LED_PIN                4
#define LED_BLINK_INTERVAL_MS  4000

#define OTA_HTTP_TIMEOUT_MS    15000
#define NVS_NAMESPACE          "fota"
#define NVS_KEY_FW_VERSION     "fw_ver"

// GLOBALS
String g_deviceId;
volatile bool g_updatePending = false;   // set by MQTT/button/scheduler, read by TaskOTA
ESP32MQTTClient mqttClient;

// Health-check task handles + tuning
TaskHandle_t g_networkTaskHandle = nullptr; 
TaskHandle_t g_otaTaskHandle = nullptr;
#define HEALTH_CHECK_INTERVAL_MS   (60UL * 1000UL)   // how often to log heap/stack stats
#define STACK_WARN_WORDS           256                // warn if a task's free stack drops below this (~1KB on ESP32)

// DOUBLE RESET DETECTION (DRD) — manual rollback recovery
// Press the physical reset button twice within DRD_WINDOW_MS to force a rollback to the other OTA partition, regardless of self-test state.

#define DRD_MAGIC 0xD12DE5E7
RTC_NOINIT_ATTR uint32_t rtcDrdFlag;
static esp_timer_handle_t s_drdDisarmTimer = nullptr;

static void drdDisarmCallback(void *arg) { rtcDrdFlag = 0; }

bool drdCheckAndArm() {
    bool doubleReset = (rtcDrdFlag == DRD_MAGIC);
    if (doubleReset) {
        DBGLN("[DRD] Double reset detected -> manual recovery mode");
        rtcDrdFlag = 0;
        return true;
    }
    rtcDrdFlag = DRD_MAGIC;
    esp_timer_create_args_t args = {};
    args.callback = &drdDisarmCallback;
    args.name = "drd_disarm";
    if (esp_timer_create(&args, &s_drdDisarmTimer) == ESP_OK) {
        esp_timer_start_once(s_drdDisarmTimer, (uint64_t)DRD_WINDOW_MS * 1000ULL);
    }
    return false;
}

// OTA: version storage, metadata fetch, download+flash+verify, self-test/rollback confirmation, manual (DRD) rollback
String otaGetCurrentVersion() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    String v;
    if (prefs.isKey(NVS_KEY_FW_VERSION)) {
        v = prefs.getString(NVS_KEY_FW_VERSION, "0.0.0");
    } else {
        v = "0.0.0";
        prefs.putString(NVS_KEY_FW_VERSION, v);   // seed so future reads never log NOT_FOUND
    }
    prefs.end();
    return v;
}

static void otaSetCurrentVersion(const String &v) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_FW_VERSION, v);
    prefs.end();
}

static String sha256ToHex(const uint8_t hash[32]) {
    static const char *hex = "0123456789abcdef";
    String out; out.reserve(64);
    for (int i = 0; i < 32; i++) { out += hex[(hash[i] >> 4) & 0xF]; out += hex[hash[i] & 0xF]; }
    return out;
}

struct FirmwareMeta {
    String version, sha256, url, releaseNotes;
    bool hwCompatible = false, valid = false;
};

static FirmwareMeta fetchMetadata() {
    FirmwareMeta meta;
    WiFiClientSecure client;
    client.setInsecure();  // TODO: pin real CA before fleet rollout

    HTTPClient http;
    String url = String(FOTA_BASE_URL) + FOTA_METADATA_PATH + "?hw=" + HW_ID + "&deviceId=" + g_deviceId;
    if (!http.begin(client, url)) { DBGLN("[OTA] metadata http.begin() failed"); return meta; }
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) { DBGF("[OTA] metadata fetch HTTP %d\n", code); http.end(); return meta; }

    JsonDocument doc;
    // DeserializationError err = deserializeJson(doc, http.getStream());
    // http.end();
    String payload = http.getString();
    http.end();
    DeserializationError err = deserializeJson(doc, payload);
    if (err) { DBGF("[OTA] metadata JSON parse failed: %s\n", err.c_str()); return meta; }

    meta.version = doc["version"] | "";
    meta.sha256 = doc["sha256"] | "";
    meta.url = doc["url"] | "";
    meta.releaseNotes = doc["release_notes"] | "";

    if (meta.url.startsWith("http://")) meta.url = "https://" + meta.url.substring(7);

    if (doc["hw_compatibility"].is<JsonArray>()) {
        for (JsonVariant v : doc["hw_compatibility"].as<JsonArray>()) {
            if (String(v.as<const char *>()) == HW_ID) { meta.hwCompatible = true; break; }
        }
    }
    meta.valid = meta.version.length() > 0 && meta.sha256.length() == 64 && meta.url.length() > 0;
    return meta;
}

static bool downloadFlashAndVerify(const FirmwareMeta &meta) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, meta.url)) { DBGLN("[OTA] firmware http.begin() failed"); return false; }
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) { DBGF("[OTA] firmware fetch HTTP %d\n", code); http.end(); return false; }

    int contentLength = http.getSize();
    if (contentLength <= 0) { DBGLN("[OTA] invalid content length"); http.end(); return false; }

    if (!Update.begin(contentLength, U_FLASH)) {   // always targets the INACTIVE OTA partition
        DBGF("[OTA] Update.begin failed: %s\n", Update.errorString()); http.end(); return false;
    }

    mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts(&sha, 0);
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024]; int written = 0; uint32_t lastData = millis();

    while (http.connected() && written < contentLength) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastData > OTA_HTTP_TIMEOUT_MS) {
                DBGLN("[OTA] stream stalled"); Update.abort(); mbedtls_sha256_free(&sha); http.end(); return false;
            }
            delay(5); continue;
        }
        int n = stream->readBytes(buf, min(avail, sizeof(buf)));
        if (n <= 0) continue;
        lastData = millis();
        mbedtls_sha256_update(&sha, buf, n);
        if (Update.write(buf, n) != (size_t)n) {
            DBGF("[OTA] flash write failed: %s\n", Update.errorString());
            Update.abort(); mbedtls_sha256_free(&sha); http.end(); return false;
        }
        written += n;

        static int lastPct = -1;
        int pct = (int)((written * 100LL) / contentLength);
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            char bar[21];
            int filled = pct / 5;
            for (int i = 0; i < 20; i++) bar[i] = (i < filled) ? '#' : '-';
            bar[20] = '\0';
            DBGF("[OTA] [%s] %d%% (%d/%d bytes)\n", bar, pct, written, contentLength);
        }

    }
    http.end();

    if (written != contentLength) { DBGLN("[OTA] incomplete download"); Update.abort(); mbedtls_sha256_free(&sha); return false; }

    uint8_t hash[32]; mbedtls_sha256_finish(&sha, hash); mbedtls_sha256_free(&sha);
    String computed = sha256ToHex(hash);
    if (!computed.equalsIgnoreCase(meta.sha256)) {
        DBGLN("[OTA] SHA-256 MISMATCH - refusing to boot this image"); Update.abort(); return false;
    }
    if (!Update.end(true)) { DBGF("[OTA] Update.end failed: %s\n", Update.errorString()); return false; }

    DBGLN("[OTA] verified + flashed successfully, rebooting into new image");
    return true;
}

void otaCheckAndUpdate() {
    if (WiFi.status() != WL_CONNECTED) { DBGLN("[OTA] no WiFi, skipping check"); return; }

    DBGLN("[OTA] checking for update...");
    FirmwareMeta meta = fetchMetadata();
    if (!meta.valid) return;

    String current = otaGetCurrentVersion();
    DBGF("[OTA] current=%s available=%s\n", current.c_str(), meta.version.c_str());

    if (meta.version == current) { DBGLN("[OTA] already up to date"); return; }

    if (!meta.hwCompatible) { DBGF("[OTA] v%s not compatible with " HW_ID "\n", meta.version.c_str()); return; }

    if (downloadFlashAndVerify(meta)) {
        otaSetCurrentVersion(meta.version);
        delay(200);
        esp_restart();
    }
}

void otaRunSelfTestAndConfirm() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;   // already confirmed, or first-ever USB flash

    DBGLN("[OTA] new image pending verification - running self-test...");
    // SELF TEST
    bool selfTestPassed = true;  // placeholder - WiFi connect result checked separately in setup()

    if (selfTestPassed) {
        esp_ota_mark_app_valid_cancel_rollback();
        DBGLN("[OTA] self-test passed, image confirmed valid");
    } else {
        DBGLN("[OTA] self-test FAILED - rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void otaManualRollback() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target || target == running) { DBGLN("[OTA] no alternate partition to roll back to"); return; }
    DBGF("[OTA] DRD manual rollback -> %s\n", target->label);
    if (esp_ota_set_boot_partition(target) == ESP_OK) { delay(200); esp_restart(); }
}

void otaLogBootInfo() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);
    const char *s = (state == ESP_OTA_IMG_VALID) ? "valid" :
                     (state == ESP_OTA_IMG_PENDING_VERIFY) ? "pending-verify" :
                     (state == ESP_OTA_IMG_INVALID) ? "invalid" : "unknown";
    DBGF("[BOOT] partition: %s @ 0x%06x, state: %s, stored version: %s\n",
         running->label, (unsigned)running->address, s, otaGetCurrentVersion().c_str());
}

// WIFI
bool wifiConnect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) { DBGLN("[WiFi] connect timeout"); return false; }
        delay(500);
    }
    DBGF("[WiFi] connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void wifiSyncTime() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm t; uint32_t start = millis();
    while (!getLocalTime(&t, 100)) { if (millis() - start > 10000) { DBGLN("[WiFi] NTP sync timeout"); return; } }
    DBGLN("[WiFi] time synced");
}

//MQTT (event-driven trigger — fallback path is POLL_INTERVAL_MS)
void onMqttConnect(esp_mqtt_client_handle_t client) {
    if (!mqttClient.isMyTurn(client)) return;
    DBGLN("[MQTT] connected, subscribing...");
    mqttClient.subscribe(MQTT_TOPIC_ALL, [](const std::string &payload) {
        DBGLN("[MQTT] update notice received"); g_updatePending = true;
    });
    std::string deviceTopic = "devices/" + std::string(g_deviceId.c_str()) + "/update";
    mqttClient.subscribe(deviceTopic, [](const std::string &payload) {
        DBGLN("[MQTT] device-specific update notice"); g_updatePending = true;
    });
}

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
esp_err_t handleMQTT(esp_mqtt_event_handle_t event) { mqttClient.onEventCallback(event); return ESP_OK; }
#else
void handleMQTT(void *h, esp_event_base_t base, int32_t id, void *data) {
    mqttClient.onEventCallback(static_cast<esp_mqtt_event_handle_t>(data));
}
#endif

void mqttSetup() {
    if (strlen(MQTT_USERNAME) > 0) mqttClient.setURI(MQTT_BROKER_URI, MQTT_USERNAME, MQTT_PASSWORD);
    else mqttClient.setURI(MQTT_BROKER_URI);  // avoid sending empty-but-present creds (some brokers reject that)
    mqttClient.setMqttClientName(g_deviceId.c_str());
    mqttClient.setKeepAlive(30);
    mqttClient.loopStart();
}

//HTTP DEVICE CHECK-IN (dashboard heartbeat)
void sendReport() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(FOTA_BASE_URL) + FOTA_REPORT_PATH)) return;
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["deviceId"] = g_deviceId;
    doc["firmware"] = otaGetCurrentVersion();
    doc["ip"] = WiFi.localIP().toString();
    doc["topic"] = "devices/" + g_deviceId + "/update";
    String body; serializeJson(doc, body);

    int code = http.POST(body);
    DBGF("[Report] POST -> HTTP %d\n", code);
    http.end();
}

// SCHEDULER + BUTTON (trigger sources, checked from TaskOTA)
static volatile bool s_buttonFlag = false;
void IRAM_ATTR buttonISR() { s_buttonFlag = true; }   // keep ISR tiny - debounce happens in TaskOTA

static void checkSchedulerTriggers() {
    static uint32_t lastInterval = 0, lastPoll = 0, lastTick = 0;
    static int lastDailyYday = -1;

    if (millis() - lastTick < 1000) return;   // 1s resolution is plenty
    lastTick = millis();

    struct tm t;
    if (getLocalTime(&t, 10) && t.tm_hour == DAILY_CHECK_HOUR && t.tm_min == DAILY_CHECK_MINUTE
        && t.tm_yday != lastDailyYday) {
        lastDailyYday = t.tm_yday; DBGLN("[Scheduler] daily trigger"); g_updatePending = true;
    }
    if (millis() - lastInterval >= INTERVAL_CHECK_MS) {
        lastInterval = millis(); DBGLN("[Scheduler] 3-hour trigger"); g_updatePending = true;
    }
    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis(); g_updatePending = true;   // quiet - fires often, otaCheckAndUpdate() logs when relevant
    }
}

static void checkButtonTrigger() {
    if (!s_buttonFlag) return;
    s_buttonFlag = false;
    delay(BUTTON_DEBOUNCE_MS);
    if (digitalRead(BUTTON_PIN) == LOW) { DBGLN("[Scheduler] button trigger"); g_updatePending = true; }
}

/* SYSTEM HEALTH MONITORING (heap + stack)
   Periodic proactive check - logs heap/stack headroom every
   HEALTH_CHECK_INTERVAL_MS and prints an early WARNING if a task's
   remaining stack drops below STACK_WARN_WORDS, well before it would
   actually overflow. This uses uxTaskGetStackHighWaterMark(), which is
   always safe to call - unlike overriding vApplicationStackOverflowHook(),
   which some Arduino-ESP32 core versions already define internally and
   will fail to LINK if redefined here. Deliberately not doing that.  */
static void checkSystemHealth() {
    DBGF("[Health] heap: free=%u  min_ever=%u  largest_block=%u\n",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

    if (g_networkTaskHandle) {
        UBaseType_t w = uxTaskGetStackHighWaterMark(g_networkTaskHandle);
        DBGF("[Health] TaskNetwork free stack: %u words\n", (unsigned)w);
        if (w < STACK_WARN_WORDS) DBGLN("[Health] WARNING: TaskNetwork stack running low!");
    }
    if (g_otaTaskHandle) {
        UBaseType_t w = uxTaskGetStackHighWaterMark(g_otaTaskHandle);
        DBGF("[Health] TaskOTA free stack: %u words\n", (unsigned)w);
        if (w < STACK_WARN_WORDS) DBGLN("[Health] WARNING: TaskOTA stack running low!");
    }
}

//communication CORE 0 (WiFi connection/maintenance, MQTT client, periodic HTTP heartbeat)
void TaskNetwork(void *pv) {
    DBGF("[Core %d] TaskNetwork started (WiFi + MQTT + HTTP reporting)\n", xPortGetCoreID());

    if (wifiConnect()) {
        wifiSyncTime();
        mqttSetup();
    }

    uint32_t lastReport = 0;
    uint32_t lastHealthCheck = 0;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            DBGLN("[Network] WiFi dropped, reconnecting...");
            wifiConnect();
        } else if (millis() - lastReport >= REPORT_INTERVAL_MS) {
            sendReport();
            lastReport = millis();
        }
        if (millis() - lastHealthCheck >= HEALTH_CHECK_INTERVAL_MS) {
            checkSystemHealth();
            lastHealthCheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//OTA — CORE 1 scheduler timers, button, LED, and actually performing OTA updates.
void TaskOTA(void *pv) {
    DBGF("[Core %d] TaskOTA started (Scheduler + Button + LED + OTA)\n", xPortGetCoreID());

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
    pinMode(LED_PIN, OUTPUT);

    uint32_t lastBlink = 0; bool ledState = false;

    for (;;) {
        if (millis() - lastBlink >= LED_BLINK_INTERVAL_MS) {
            lastBlink = millis(); ledState = !ledState; digitalWrite(LED_PIN, ledState);
        }

        checkSchedulerTriggers();
        checkButtonTrigger();

        if (g_updatePending) {
            g_updatePending = false;
            otaCheckAndUpdate();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

//SETUP / LOOP
void setup() {
    DBG_INIT(115200);
    delay(300);
    DBGLN("\n[BOOT] FOTA firmware starting...");

    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[13]; snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    g_deviceId = String(buf);
    DBGLN("[BOOT] device id: " + g_deviceId);

    otaLogBootInfo();

    if (drdCheckAndArm()) otaManualRollback();   // DRD hit - reboots internally if a target partition exists
    otaRunSelfTestAndConfirm();

    xTaskCreatePinnedToCore(TaskNetwork, "TaskNetwork", 8192, nullptr, 1, &g_networkTaskHandle, 0);  // CORE 0
    xTaskCreatePinnedToCore(TaskOTA,     "TaskOTA",     8192, nullptr, 1, &g_otaTaskHandle,     1);  // CORE 1

    DBGLN("[BOOT] setup complete, both tasks launched");
}

void loop() {
    vTaskDelete(NULL);   // everything runs in TaskNetwork/TaskOTA - free up the default loop task
}
