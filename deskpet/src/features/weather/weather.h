#pragma once
#include <stdint.h>

enum class WeatherError : uint8_t { None, Network, Http, Json, Schema, Clock };

struct WeatherResult {
  uint8_t place = 0;
  WeatherError error = WeatherError::Network;
  float temperature = 0;
  int humidity = 0;
  int code = 0;
  uint32_t fetchedAt = 0;
};

// Starts the background task that runs HTTPS requests away from the UI loop.
bool startWeatherWorker();
// Queues a fetch for PLACES[place] without blocking; false if one is already queued.
bool requestWeatherFetch(uint8_t place);
// Takes the most recent finished fetch, if there is one.
bool takeWeatherResult(WeatherResult &result);
