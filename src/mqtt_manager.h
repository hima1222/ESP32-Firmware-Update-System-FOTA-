#pragma once
#include <Arduino.h>

void mqttManagerInit();

void mqttPublishStatus(const String &status);
