#pragma once
#include <Arduino.h>

void reportInit();
void reportTick();

void reportNow(const String &status);
