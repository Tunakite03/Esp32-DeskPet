#include "touch_input.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include "config.h"
#include "log.h"

namespace {
constexpr uint8_t TOUCH_ADDRESS = 0x38;  // FT6336
constexpr uint32_t SAMPLE_INTERVAL = 10;
constexpr uint8_t FAILURES_BEFORE_RELEASE = 5;
constexpr uint32_t MAX_BACKOFF = 500;
// A healthy read takes ~0.3 ms; a short timeout bounds how long a stuck bus can hold us.
constexpr uint16_t I2C_TIMEOUT = 5;

QueueHandle_t samples = nullptr;
std::atomic<uint8_t> failures{0};

bool readController(bool &pressed, int16_t &x, int16_t &y) {
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(static_cast<int>(TOUCH_ADDRESS), 5) != 5) {
    while (Wire.available()) Wire.read();
    return false;
  }
  uint8_t data[5];
  for (uint8_t &value : data) value = Wire.read();
  pressed = (data[0] & 0x0F) > 0;
  if (!pressed) return true;
  // The panel reports portrait coordinates; the UI runs in landscape.
  const int rawX = ((data[1] & 0x0F) << 8) | data[2];
  const int rawY = ((data[3] & 0x0F) << 8) | data[4];
  if (rawX >= SCREEN_HEIGHT || rawY >= SCREEN_WIDTH) return false;
  x = rawY;
  y = SCREEN_HEIGHT - 1 - rawX;
  return true;
}

// The touch controller is the only I2C user after boot, so the bus can be
// restarted here when it stops answering.
void restartBus() {
  Wire.end();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  Wire.setTimeOut(I2C_TIMEOUT);
}

void publish(const TouchSample &sample) {
  // A full queue means the main loop is stalled; dropping the oldest move keeps
  // the latest position, and presses/releases are rare enough to always fit.
  if (xQueueSend(samples, &sample, 0) != pdTRUE) {
    TouchSample oldest;
    xQueueReceive(samples, &oldest, 0);
    xQueueSend(samples, &sample, 0);
  }
}

void touchWorker(void *) {
  TouchSample last{0, 0, 0, false};
  for (;;) {
    // Always sleep, even when a read was slow: this task outranks the Arduino loop on
    // core 1, so a busy loop here (e.g. I2C timing out) would freeze the whole UI.
    // A controller that stops answering is polled less and less often.
    const uint8_t failed = failures.load();
    const uint32_t pause = failed == 0 ? SAMPLE_INTERVAL : min<uint32_t>(MAX_BACKOFF, 50u << min<uint8_t>(failed - 1, 3));
    vTaskDelay(pdMS_TO_TICKS(pause));
    bool pressed = false;
    int16_t x = last.x, y = last.y;
    if (!readController(pressed, x, y)) {
      const uint8_t count = failed < 255 ? failures.fetch_add(1) + 1 : 255;
      if (count == FAILURES_BEFORE_RELEASE) {
        logError("touch", "FT6336 read failed");
        // Never leave a finger "stuck" down when the controller stops answering.
        if (last.pressed) { last = {millis(), last.x, last.y, false}; publish(last); }
        restartBus();
      }
      continue;
    }
    failures.store(0);
    if (pressed == last.pressed && (!pressed || (x == last.x && y == last.y))) continue;
    last = {millis(), x, y, pressed};
    publish(last);
  }
}
}  // namespace

bool startTouchInput() {
  samples = xQueueCreate(32, sizeof(TouchSample));
  Wire.setTimeOut(I2C_TIMEOUT);
  // Above the Arduino loop (priority 1) on the same core, so drawing never delays sampling.
  return samples && xTaskCreatePinnedToCore(touchWorker, "touch", 3072, nullptr, 3, nullptr, 1) == pdPASS;
}

bool nextTouchSample(TouchSample &sample) { return samples && xQueueReceive(samples, &sample, 0) == pdTRUE; }

uint8_t touchFailures() { return failures.load(); }
