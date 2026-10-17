#pragma once
#include <stdint.h>

enum class WeatherError : uint8_t { None, Network, Http, Json, Schema, Clock };

// Hourly chance of rain for the current hour and the next five, in local time.
constexpr uint8_t RAIN_HOURS = 6;
struct HourlyRain {
  uint8_t hour = 0;    // 0-23
  uint8_t chance = 0;  // percent
};

struct WeatherResult {
  uint8_t place = 0;
  WeatherError error = WeatherError::Network;
  float temperature = 0;
  int humidity = 0;
  HourlyRain rainByHour[RAIN_HOURS];
  int code = 0;
  uint32_t fetchedAt = 0;
};

// Starts the background task that runs HTTPS requests away from the UI loop.
bool startWeatherWorker();
// Queues a fetch for PLACES[place] without blocking; false if one is already queued.
bool requestWeatherFetch(uint8_t place);
// Takes the most recent finished fetch, if there is one.
bool takeWeatherResult(WeatherResult &result);
