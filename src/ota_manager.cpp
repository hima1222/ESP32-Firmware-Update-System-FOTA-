#include "ota_manager.h"
#include "config.h"
#include "wifi_manager.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

extern String g_deviceId;  

static int compareVersions(const String &a, const String &b) {
    int aMaj, aMin, aPatch, bMaj, bMin, bPatch;
    sscanf(a.c_str(), "%d.%d.%d", &aMaj, &aMin, &aPatch);
    sscanf(b.c_str(), "%d.%d.%d", &bMaj, &bMin, &bPatch);
    if (aMaj != bMaj) return aMaj > bMaj ? 1 : -1;
    if (aMin != bMin) return aMin > bMin ? 1 : -1;
    if (aPatch != bPatch) return aPatch > bPatch ? 1 : -1;
    return 0;
}

static String sha256ToHex(const uint8_t hash[32]) {
    static const char *hexChars = "0123456789abcdef";
    String out;
    out.reserve(64);
    for (int i = 0; i < 32; i++) {
        out += hexChars[(hash[i] >> 4) & 0xF];
        out += hexChars[hash[i] & 0xF];
    }
    return out;
}


String otaGetCurrentVersion() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);

    String v;
    if (prefs.isKey(NVS_KEY_FW_VERSION)) {
        v = prefs.getString(NVS_KEY_FW_VERSION, "0.0.0");
    } else {
        v = "0.0.0";
        prefs.putString(NVS_KEY_FW_VERSION, v);
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

// Metadata fetch

struct FirmwareMeta {
    String version;
    String sha256;
    String url;
    String releaseNotes;
    bool hwCompatible = false;
    bool valid = false;
};

static FirmwareMeta fetchMetadata() {
    FirmwareMeta meta;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    String url = String(FOTA_BASE_URL) + FOTA_METADATA_PATH + "?hw=" + HW_ID + "&deviceId=" + g_deviceId;
    if (!http.begin(client, url)) {
        Serial.println("[OTA] metadata http.begin() failed");
        return meta;
    }
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] metadata fetch HTTP %d\n", code);
        http.end();
        return meta;
    }

    JsonDocument doc;  
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
        Serial.printf("[OTA] metadata JSON parse failed: %s\n", err.c_str());
        return meta;
    }

    meta.version = doc["version"] | "";
    meta.sha256 = doc["sha256"] | "";
    meta.url = doc["url"] | "";
    meta.releaseNotes = doc["release_notes"] | "";

    if (meta.url.startsWith("http://")) {
        meta.url = "https://" + meta.url.substring(7);  // strlen("http://") == 7
    }

    meta.hwCompatible = false;
    if (doc["hw_compatibility"].is<JsonArray>()) {
        for (JsonVariant v : doc["hw_compatibility"].as<JsonArray>()) {
            if (String(v.as<const char *>()) == HW_ID) {
                meta.hwCompatible = true;
                break;
            }
        }
    }

    meta.valid = meta.version.length() > 0 && meta.sha256.length() == 64 && meta.url.length() > 0;
    return meta;
}


static bool downloadFlashAndVerify(const FirmwareMeta &meta) {
    WiFiClientSecure client;
    client.setInsecure();  

    HTTPClient http;
    if (!http.begin(client, meta.url)) {
        Serial.println("[OTA] firmware http.begin() failed");
        return false;
    }
    http.setTimeout(OTA_HTTP_TIMEOUT_MS);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] firmware fetch HTTP %d\n", code);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("[OTA] unknown/invalid content length");
        http.end();
        return false;
    }

    if (!Update.begin(contentLength, U_FLASH)) {
        Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        http.end();
        return false;
    }

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);  

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    uint32_t lastData = millis();

    while (http.connected() && written < contentLength) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (millis() - lastData > OTA_HTTP_TIMEOUT_MS) {
                Serial.println("[OTA] stream stalled, aborting");
                Update.abort();
                mbedtls_sha256_free(&shaCtx);
                http.end();
                return false;
            }
            delay(5);
            continue;
        }
        size_t toRead = min(avail, sizeof(buf));
        int n = stream->readBytes(buf, toRead);
        if (n <= 0) continue;

        lastData = millis();
        mbedtls_sha256_update(&shaCtx, buf, n);

        if (Update.write(buf, n) != (size_t)n) {
            Serial.printf("[OTA] flash write failed: %s\n", Update.errorString());
            Update.abort();
            mbedtls_sha256_free(&shaCtx);
            http.end();
            return false;
        }
        written += n;
    }
    http.end();

    if (written != contentLength) {
        Serial.printf("[OTA] incomplete download: %d/%d bytes\n", written, contentLength);
        Update.abort();
        mbedtls_sha256_free(&shaCtx);
        return false;
    }

    uint8_t hash[32];
    mbedtls_sha256_finish(&shaCtx, hash);
    mbedtls_sha256_free(&shaCtx);
    String computedHex = sha256ToHex(hash);

    if (!computedHex.equalsIgnoreCase(meta.sha256)) {
        Serial.println("[OTA] SHA-256 MISMATCH - refusing to boot this image");
        Serial.println("  expected: " + meta.sha256);
        Serial.println("  computed: " + computedHex);
        Update.abort();
        return false;
    }

    if (!Update.end(true)) {
        Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        return false;
    }

    Serial.println("[OTA] verified + flashed successfully, rebooting into new image");
    return true;
}


OtaResult otaCheckAndUpdate() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] no WiFi, skipping check");
        return OtaResult::ERROR;
    }

    Serial.println("[OTA] checking for update...");
    FirmwareMeta meta = fetchMetadata();
    if (!meta.valid) {
        return OtaResult::METADATA_FETCH_FAILED;
    }

    String current = otaGetCurrentVersion();
    Serial.printf("[OTA] current=%s available=%s\n", current.c_str(), meta.version.c_str());

    if (compareVersions(meta.version, current) <= 0) {
        Serial.println("[OTA] already up to date");
        return OtaResult::UP_TO_DATE;
    }

    if (!meta.hwCompatible) {
        Serial.println("[OTA] server has a newer version but it's not compatible with " HW_ID);
        return OtaResult::HW_INCOMPATIBLE;
    }

    if (!downloadFlashAndVerify(meta)) {
        return OtaResult::DOWNLOAD_FAILED;  
    }

    otaSetCurrentVersion(meta.version);

    delay(200);
    esp_restart();
    return OtaResult::UPDATED_REBOOTING;  
}

void otaRunSelfTestAndConfirm() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        // Already confirmed valid in a previous boot - nothing to do.
        return;
    }

    Serial.println("[OTA] new image pending verification - running self-test...");

    // SELF-TEST HOOK 
    bool selfTestPassed = wifiIsConnected();
    
    if (selfTestPassed) {
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.println("[OTA] self-test passed, image confirmed valid");
    } else {
        Serial.println("[OTA] self-test FAILED - rolling back to previous image");
        esp_ota_mark_app_invalid_rollback_and_reboot();  
    }
}

void otaLogBootInfo() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);

    const char *stateStr = "unknown";
    switch (state) {
        case ESP_OTA_IMG_VALID:          stateStr = "valid (confirmed)"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: stateStr = "pending-verify (rollback armed)"; break;
        case ESP_OTA_IMG_INVALID:        stateStr = "invalid"; break;
        case ESP_OTA_IMG_ABORTED:        stateStr = "aborted"; break;
        default: break;
    }

    Serial.printf("[BOOT] running partition: %s @ 0x%06x, size %u, state: %s\n",
                  running->label, (unsigned)running->address, (unsigned)running->size, stateStr);
    Serial.println("[BOOT] stored firmware version: " + otaGetCurrentVersion());
}

void otaManualRollback() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);

    if (target == nullptr || target == running) {
        Serial.println("[OTA] no alternate OTA partition available to roll back to");
        return;
    }

    Serial.printf("[OTA] DRD manual rollback: switching boot partition to %s\n", target->label);
    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        Serial.printf("[OTA] esp_ota_set_boot_partition failed: %d\n", err);
        return;
    }
    delay(200);
    esp_restart();
}
