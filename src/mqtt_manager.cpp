#include "mqtt_manager.h"
#include "config.h"
#include "update_trigger.h"
#include <ESP32MQTTClient.h>
#include <cstring>

extern String g_deviceId; 

ESP32MQTTClient mqttClient;


void onMqttConnect(esp_mqtt_client_handle_t client) {
    if (!mqttClient.isMyTurn(client)) return;

    Serial.println("[MQTT] connected, subscribing...");

    mqttClient.subscribe(MQTT_TOPIC_ALL, [](const std::string &payload) {
        Serial.printf("[MQTT] update notice on %s: %s\n", MQTT_TOPIC_ALL, payload.c_str());
        raiseUpdateTrigger(UpdateReason::MQTT_EVENT);
    });

    std::string deviceTopic = "devices/" + std::string(g_deviceId.c_str()) + "/update";
    mqttClient.subscribe(deviceTopic, [](const std::string &payload) {
        Serial.printf("[MQTT] device-specific update notice: %s\n", payload.c_str());
        raiseUpdateTrigger(UpdateReason::MQTT_EVENT);
    });
}

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
esp_err_t handleMQTT(esp_mqtt_event_handle_t event) {
    mqttClient.onEventCallback(event);
    return ESP_OK;
}
#else
void handleMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
    mqttClient.onEventCallback(event);
}
#endif


void mqttManagerInit() {
    if (strlen(MQTT_USERNAME) > 0) {
        mqttClient.setURI(MQTT_BROKER_URI, MQTT_USERNAME, MQTT_PASSWORD);
    } else {
        mqttClient.setURI(MQTT_BROKER_URI);
    }
    mqttClient.setMqttClientName(g_deviceId.c_str());
    mqttClient.setKeepAlive(30);


    mqttClient.loopStart();  
}

void mqttPublishStatus(const String &status) {
    if (!mqttClient.isConnected()) return;
    std::string topic = "devices/" + std::string(g_deviceId.c_str()) + "/status";
    mqttClient.publish(topic, status.c_str(), 1, false);
}
