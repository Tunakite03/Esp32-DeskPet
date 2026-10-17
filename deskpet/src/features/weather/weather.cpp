#include "src/features/weather/weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <lwip/netdb.h>
#include <math.h>
#include "src/core/system/config.h"
#include "src/core/system/timekeeping.h"

namespace {
constexpr char WEATHER_HOST[] = "api.open-meteo.com";
constexpr uint32_t NETWORK_TIMEOUT = 6000;

struct WeatherRequest { uint8_t place; };
QueueHandle_t weatherRequests = nullptr;
QueueHandle_t weatherResults = nullptr;

bool plausible(const WeatherResult &result) {
  return isfinite(result.temperature) && result.temperature >= -90 && result.temperature <= 65 &&
         result.humidity >= 0 && result.humidity <= 100 && result.rainChance >= 0 && result.rainChance <= 100 &&
         result.code >= 0 && result.code <= 99;
}

// Network.hostByName() (used by HTTPClient) clears the lwIP DNS cache without
// holding the TCP/IP core lock in arduino-esp32 3.3. If SNTP has a lookup in
// flight at that moment, lwIP asserts and the board resets, which happens
// right after Wi-Fi connects when the RTC kept the time across a soft reset.
// lwip_getaddrinfo() goes through the TCP/IP thread and is safe to call here.
bool resolveHost(const char *host, IPAddress &address) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *found = nullptr;
  if (lwip_getaddrinfo(host, nullptr, &hints, &found) != 0 || !found) return false;
  address = IPAddress(reinterpret_cast<const sockaddr_in *>(found->ai_addr)->sin_addr.s_addr);
  lwip_freeaddrinfo(found);
  return true;
}

// Sends an HTTP/1.0 GET (so the body is never chunked) and skips the response
// headers. Returns the status code, or -1 when the TLS connection failed.
// The server is verified against the ESP-IDF root store (Mozilla's CA list built
// into the core), so a change of certificate authority does not need a reflash.
int httpsGet(NetworkClientSecure &client, const IPAddress &address, const String &path) {
  client.useBuiltinCACertBundle();
  if (!client.connect(address, 443, WEATHER_HOST, nullptr, nullptr, nullptr)) return -1;
  client.print("GET " + path + " HTTP/1.0\r\nHost: " + WEATHER_HOST +
               "\r\nUser-Agent: DeskPet\r\nConnection: close\r\n\r\n");
  const String statusLine = client.readStringUntil('\n');
  const int status = statusLine.startsWith("HTTP/") ? statusLine.substring(statusLine.indexOf(' ') + 1).toInt() : 0;
  while (client.connected() || client.available()) {
    const String header = client.readStringUntil('\n');
    if (header.length() <= 1) break;  // the blank "\r" line ends the headers
  }
  return status;
}

WeatherResult fetchWeather(uint8_t requestedPlace) {
  WeatherResult result;
  result.place = requestedPlace;
  if (!clockReady()) { result.error = WeatherError::Clock; return result; }
  const Place &location = PLACES[requestedPlace];
  const String path = "/v1/forecast?latitude=" + String(location.latitude, 4) +
                      "&longitude=" + String(location.longitude, 4) +
                      "&current=temperature_2m,relative_humidity_2m,weather_code"
                      "&daily=precipitation_probability_max&timezone=auto&forecast_days=1";
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    if (WiFi.status() != WL_CONNECTED) { result.error = WeatherError::Network; return result; }
    int status = 0;
    IPAddress address;
    NetworkClientSecure client;
    client.setHandshakeTimeout(8);
    client.setConnectionTimeout(NETWORK_TIMEOUT);
    client.setTimeout(NETWORK_TIMEOUT);
    if (!resolveHost(WEATHER_HOST, address)) {
      result.error = WeatherError::Network;
    } else if ((status = httpsGet(client, address, path)) != 200) {
      result.error = WeatherError::Http;
    } else {
      // Keep only the fields read below, so extra data in the response costs no heap.
      JsonDocument filter;
      filter["current"]["temperature_2m"] = true;
      filter["current"]["relative_humidity_2m"] = true;
      filter["current"]["weather_code"] = true;
      filter["daily"]["precipitation_probability_max"] = true;
      JsonDocument json;
      const DeserializationError parseError =
          deserializeJson(json, client, DeserializationOption::Filter(filter));
      if (parseError) {
        result.error = WeatherError::Json;
      } else {
        const JsonObjectConst current = json["current"].as<JsonObjectConst>();
        const JsonVariantConst rainChance = json["daily"]["precipitation_probability_max"][0];
        if (!current["temperature_2m"].is<float>() || !current["relative_humidity_2m"].is<int>() ||
            !current["weather_code"].is<int>() || !rainChance.is<int>()) {
          result.error = WeatherError::Schema;
        } else {
          result.temperature = current["temperature_2m"].as<float>();
          result.humidity = current["relative_humidity_2m"].as<int>();
          result.code = current["weather_code"].as<int>();
          result.rainChance = rainChance.as<int>();
          result.error = plausible(result) ? WeatherError::None : WeatherError::Schema;
          result.fetchedAt = millis();
        }
      }
    }
    client.stop();
    if (result.error == WeatherError::None) return result;
    Serial.printf("{\"level\":\"warning\",\"component\":\"weather\",\"attempt\":%u,"
                  "\"http_status\":%d,\"error_code\":%u}\n",
                  attempt, status, static_cast<unsigned>(result.error));
    if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(attempt * 1000));
  }
  return result;
}

void weatherWorker(void *) {
  WeatherRequest request{};
  for (;;) {
    if (xQueueReceive(weatherRequests, &request, portMAX_DELAY) == pdTRUE) {
      const WeatherResult result = fetchWeather(request.place);
      xQueueOverwrite(weatherResults, &result);
    }
  }
}
}  // namespace

bool startWeatherWorker() {
  weatherRequests = xQueueCreate(1, sizeof(WeatherRequest));
  weatherResults = xQueueCreate(1, sizeof(WeatherResult));
  return weatherRequests && weatherResults &&
         xTaskCreatePinnedToCore(weatherWorker, "weather", 12288, nullptr, 1, nullptr, 0) == pdPASS;
}

bool requestWeatherFetch(uint8_t place) {
  const WeatherRequest request{place};
  return xQueueSend(weatherRequests, &request, 0) == pdTRUE;
}

bool takeWeatherResult(WeatherResult &result) {
  return xQueueReceive(weatherResults, &result, 0) == pdTRUE;
}
