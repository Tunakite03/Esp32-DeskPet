#pragma once
#include <Arduino.h>

inline void logError(const char *component, const char *reason) {
  Serial.printf("{\"level\":\"error\",\"component\":\"%s\",\"reason\":\"%s\"}\n", component, reason);
}
