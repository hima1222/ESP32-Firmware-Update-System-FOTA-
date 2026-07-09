#pragma once
#include <Arduino.h>

#define HW_ID "esp32-devkit-v1"

// WIFI 
#define WIFI_CONNECT_TIMEOUT_MS   15000   
#define WIFI_RETRY_DELAY_MS       500

//  BACKEND  
#define FOTA_BASE_URL         "https://fotaproject-production.up.railway.app"
#define FOTA_METADATA_PATH    "/api/firmware/latest"   
#define FOTA_REPORT_PATH      "/api/esp/data"           

// MQTT (event-driven trigger channel)
#define MQTT_BROKER_URI       "mqtt://test.mosquitto.org:1883"  
#define MQTT_USERNAME         ""  
#define MQTT_PASSWORD         ""
#define MQTT_TOPIC_ALL        "devices/all/update"
// device-specific topic is built at runtime: "devices/<deviceId>/update"
#define MQTT_TOPIC_STATUS_FMT "devices/%s/status"      

// SCHEDULER
#define NTP_SERVER              "pool.ntp.org"
#define GMT_OFFSET_SEC          (5 * 3600 + 1800)   // Sri Lanka UTC+5:30
#define DAYLIGHT_OFFSET_SEC     0
#define DAILY_CHECK_HOUR        0     // 12:00 AM
#define DAILY_CHECK_MINUTE      0
#define INTERVAL_CHECK_MS       (3UL * 60UL * 60UL * 1000UL)  // every 3 hours
#define POLL_INTERVAL_MS        (20UL * 1000UL) //20sec

// PUSH BUTTON (manual "check now" + DRD manual rollback)
#define BUTTON_PIN              0      
#define BUTTON_DEBOUNCE_MS      50

// DOUBLE RESET DETECTION (manual rollback recovery)
#define DRD_WINDOW_MS           3000   // press physical reset twice within 3s


// TEST LED 

#define LED_PIN                 4    
#define LED_BLINK_INTERVAL_MS   2000   

#define OTA_HTTP_TIMEOUT_MS     15000
#define OTA_SELFTEST_TIMEOUT_MS 10000
#define NVS_NAMESPACE           "fota"
#define NVS_KEY_FW_VERSION      "fw_ver"
