#include "src/core/audio/audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <math.h>
#include <driver/i2s_std.h>
#include <freertos/semphr.h>
#include "src/core/audio/ringtone_data.h"
#include "src/core/system/config.h"
#include "src/core/system/log.h"

// ES8311 register configuration follows the 16 kHz / 256x MCLK mode documented in:
// https://github.com/Freenove/Freenove_ESP32_S3_Display/tree/main/Tutorial_With_Touch/Sketches/Sketch_07.1_Music
// Microphone settings (REG14/16/17) follow Sketch_07.2_Echo from the same repository.
namespace {
constexpr uint8_t CODEC_ADDRESS = 0x18;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint8_t MIC_GAIN = 0x05;  // REG16: 6 dB per step, 0x05 = +30 dB for speech across a desk

enum class AudioSource : uint8_t { Ringtone, Recording, Music, Alarm };
struct AudioRequest {
  uint32_t generation;
  const int16_t *samples;  // mono 16 kHz
  uint32_t count;
  bool loop;  // start again from the first sample until the request goes stale
  AudioSource source = AudioSource::Ringtone;
};

struct StreamBlock {
  uint32_t generation;
  uint16_t count;
  int16_t samples[256];
};

i2s_chan_handle_t speakerChannel = nullptr;
i2s_chan_handle_t micChannel = nullptr;
QueueHandle_t audioRequests = nullptr;
SemaphoreHandle_t requestMutex = nullptr;
AudioSource focus = AudioSource::Ringtone;  // protected by requestMutex
bool workerActive = false;  // protected by requestMutex
QueueHandle_t streamBlocks = nullptr;
std::atomic<uint32_t> streamToken{0}, streamDone{0}, streamFrames{0};
std::atomic<bool> streamPaused{false};
std::atomic<bool> audioReady{false};
std::atomic<bool> micAvailable{false};
std::atomic<uint8_t> audioGain{40};
std::atomic<uint32_t> audioCompleted{0};
std::atomic<uint32_t> audioPeak{0};
std::atomic<uint16_t> playbackPeak{0};
std::atomic<bool> audioCancelled{false};
std::atomic<bool> busy{false};
// Every request carries a new generation so the worker drops anything older.
std::atomic<uint32_t> audioGeneration{0};
// Set once at boot, before any request reaches the worker.
const int16_t *ringtoneSamples = RINGTONE_PCM;
uint32_t ringtoneCount = RINGTONE_SAMPLES;

bool codecWrite(uint8_t address, uint8_t value) {
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    Wire.beginTransmission(CODEC_ADDRESS);
    Wire.write(address); Wire.write(value);
    if (Wire.endTransmission() == 0) return true;
    Serial.printf("{\"level\":\"warning\",\"component\":\"codec\",\"register\":%u,\"attempt\":%u}\n",
                  address, attempt);
    delay(2);
  }
  logError("codec", "ES8311 register write failed after retries");
  return false;
}

bool writeAudioSamples(const int16_t *samples, size_t count) {
  size_t written = 0;
  const size_t bytes = count * sizeof(int16_t);
  const esp_err_t result = i2s_channel_write(speakerChannel, samples, bytes, &written, 300);
  if (result == ESP_OK && written == bytes) return true;
  digitalWrite(PIN_SPEAKER_MUTE, HIGH);
  audioReady.store(false);
  Serial.printf("{\"level\":\"error\",\"component\":\"audio\",\"reason\":\"I2S transfer failed\","
                "\"code\":%d,\"bytes_written\":%u}\n",
                result, static_cast<unsigned>(written));
  return false;
}

bool requestStale(const AudioRequest &request) {
  return (request.source != AudioSource::Music && audioGain.load() == 0) || audioCancelled.load() ||
         request.generation != audioGeneration.load();
}

void finishRequest() {
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  workerActive = false;
  playbackPeak.store(0);
  if (uxQueueMessagesWaiting(audioRequests) == 0) busy.store(false);
  xSemaphoreGive(requestMutex);
}

// Streams one request in 256-frame blocks so a newer request can cut in quickly.
// A looping request starts again from the first sample until it goes stale.
bool playRequest(const AudioRequest &request, int16_t *samples, uint32_t &peak) {
  uint32_t index = 0;
  for (;;) {
    if (requestStale(request)) return false;
    if (!request.loop && index >= request.count) return true;
    const float volume = audioGain.load() / 100.0F;
    uint32_t blockPeak = 0;
    for (uint32_t frame = 0; frame < 256; ++frame) {
      const float source = index < request.count ? request.samples[index] : 0;
      if (++index == request.count && request.loop) index = 0;
      const int16_t value = static_cast<int16_t>(source * volume * volume);
      samples[frame * 2] = value; samples[frame * 2 + 1] = value;
      blockPeak = max(blockPeak, static_cast<uint32_t>(abs(static_cast<int>(value))));
    }
    playbackPeak.store(static_cast<uint16_t>(min<uint32_t>(blockPeak, UINT16_MAX)));
    peak = max(peak, blockPeak);
    if (!writeAudioSamples(samples, 512)) return false;
  }
}

bool playStream(const AudioRequest &request, int16_t *samples, uint32_t &peak) {
  StreamBlock block{};
  for (;;) {
    if (requestStale(request) || !audioReady.load()) return false;
    if (streamPaused.load()) {
      digitalWrite(PIN_SPEAKER_MUTE, HIGH);
      playbackPeak.store(0);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (xQueueReceive(streamBlocks, &block, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (block.generation != request.generation) continue;
    if (block.count == 0) return true;
    const float volume = audioGain.load() / 100.0F;
    uint32_t blockPeak = 0;
    for (uint16_t index = 0; index < block.count; ++index) {
      const int16_t value = static_cast<int16_t>(block.samples[index] * volume * volume);
      samples[index * 2] = value;
      samples[index * 2 + 1] = value;
      blockPeak = max(blockPeak, static_cast<uint32_t>(abs(static_cast<int>(value))));
    }
    if (requestStale(request)) return false;
    digitalWrite(PIN_SPEAKER_MUTE, audioGain.load() == 0 || streamPaused.load() ? HIGH : LOW);
    if (!writeAudioSamples(samples, block.count * 2)) return false;
    xSemaphoreTake(requestMutex, portMAX_DELAY);
    if (request.generation == streamToken.load()) streamFrames.fetch_add(block.count);
    xSemaphoreGive(requestMutex);
    peak = max(peak, blockPeak);
    playbackPeak.store(static_cast<uint16_t>(blockPeak));
  }
}

void audioWorker(void *) {
  AudioRequest request{};
  int16_t samples[512]{};
  for (;;) {
    if (xQueueReceive(audioRequests, &request, portMAX_DELAY) != pdTRUE) continue;
    xSemaphoreTake(requestMutex, portMAX_DELAY);
    workerActive = true;
    xSemaphoreGive(requestMutex);
    if (!audioReady.load() || requestStale(request)) { finishRequest(); continue; }
    // Prime the DMA buffer with silence before enabling the amplifier to avoid a pop.
    memset(samples, 0, sizeof(samples));
    if (!writeAudioSamples(samples, 512)) { finishRequest(); continue; }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (requestStale(request)) { finishRequest(); continue; }
    digitalWrite(PIN_SPEAKER_MUTE, audioGain.load() == 0 ? HIGH : LOW);
    uint32_t peak = 0;
    const bool complete = request.source == AudioSource::Music ? playStream(request, samples, peak) : playRequest(request, samples, peak);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(PIN_SPEAKER_MUTE, HIGH);
    if (complete) {
      audioPeak.store(peak);
      audioCompleted.fetch_add(1);
      Serial.println(request.source == AudioSource::Music ? "{\"event\":\"music_complete\"}" : request.samples == ringtoneSamples ?
                     "{\"event\":\"sound_complete\"}" : "{\"event\":\"recording_complete\"}");
    }
    if (request.source == AudioSource::Music) streamDone.store(request.generation);
    xSemaphoreTake(requestMutex, portMAX_DELAY);
    if (request.generation == audioGeneration.load()) focus = AudioSource::Ringtone;
    xSemaphoreGive(requestMutex);
    finishRequest();
  }
}

void queueRequest(const int16_t *samples, uint32_t count, AudioSource source) {
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  if ((focus == AudioSource::Alarm && source != AudioSource::Alarm) ||
      (focus == AudioSource::Music && source == AudioSource::Recording)) {
    xSemaphoreGive(requestMutex);
    return;
  }
  focus = source;
  audioCancelled.store(false);
  busy.store(true);
  const AudioRequest event{audioGeneration.fetch_add(1) + 1, samples, count, source == AudioSource::Alarm, source};
  if (xQueueOverwrite(audioRequests, &event) != pdTRUE) {
    busy.store(false);
    logError("audio", "Unable to queue sound");
  }
  xSemaphoreGive(requestMutex);
}
}  // namespace

bool initAudio() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  channel.auto_clear = true;
  // One full-duplex port: the speaker and the microphone share MCLK, BCLK and WS.
  if (i2s_new_channel(&channel, &speakerChannel, &micChannel) != ESP_OK) {
    logError("audio", "I2S channel allocation failed"); return false;
  }
  i2s_std_config_t config{};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = GPIO_NUM_4;
  config.gpio_cfg.bclk = GPIO_NUM_5;
  config.gpio_cfg.ws = GPIO_NUM_7;
  config.gpio_cfg.dout = GPIO_NUM_8;
  config.gpio_cfg.din = GPIO_NUM_6;
  if (i2s_channel_init_std_mode(speakerChannel, &config) != ESP_OK ||
      i2s_channel_init_std_mode(micChannel, &config) != ESP_OK ||
      i2s_channel_enable(speakerChannel) != ESP_OK) {
    logError("audio", "I2S clock or output initialization failed"); return false;
  }
  if (!codecWrite(0x00, 0x1F)) return false;
  delay(20);
  constexpr uint8_t registers[][2] = {
    {0x00, 0x00}, {0x00, 0x80}, {0x01, 0x3F}, {0x02, 0x00},
    {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00}, {0x06, 0x03},
    {0x07, 0x00}, {0x08, 0xFF}, {0x09, 0x0C}, {0x0A, 0x0C},
    {0x0D, 0x01}, {0x0E, 0x02}, {0x12, 0x00}, {0x13, 0x10},
    {0x14, 0x1A}, {0x16, MIC_GAIN}, {0x17, 0xC8},
    {0x1C, 0x6A}, {0x37, 0x08}, {0x32, 0xBF}, {0x31, 0x00}
  };
  for (const auto &entry : registers) if (!codecWrite(entry[0], entry[1])) return false;
  Wire.beginTransmission(CODEC_ADDRESS); Wire.write(0x32);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(static_cast<int>(CODEC_ADDRESS), 1) != 1 ||
      Wire.read() != 0xBF) {
    logError("codec", "DAC configuration readback failed"); return false;
  }
  audioRequests = xQueueCreate(1, sizeof(AudioRequest));
  streamBlocks = xQueueCreate(12, sizeof(StreamBlock));
  requestMutex = xSemaphoreCreateMutex();
  if (!audioRequests || !streamBlocks || !requestMutex || xTaskCreatePinnedToCore(audioWorker, "speaker", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    logError("audio", "Speaker worker allocation failed"); return false;
  }
  audioReady.store(true);
  // The speaker still works if only the microphone fails.
  if (i2s_channel_enable(micChannel) == ESP_OK) micAvailable.store(true);
  else logError("audio", "Microphone channel could not be enabled");
  Serial.printf("{\"event\":\"audio_ready\",\"codec\":\"ES8311\",\"sample_rate\":16000,\"microphone\":%s}\n",
                micAvailable.load() ? "true" : "false");
  return true;
}

bool audioIsReady() { return audioReady.load(); }
bool micReady() { return micAvailable.load(); }

void setAudioGain(uint8_t gain) {
  audioGain.store(gain);
  if (gain == 0) digitalWrite(PIN_SPEAKER_MUTE, HIGH);
}

void setRingtone(const int16_t *samples, uint32_t count) {
  if (!samples || count == 0) { logError("audio", "Ignored an empty ringtone"); return; }
  ringtoneSamples = samples;
  ringtoneCount = count;
}

// Plays the ringtone once. Used by the test buttons and the microphone test.
void requestSound() {
  if (!audioReady.load()) { logError("audio", "Speaker is unavailable"); return; }
  if (audioGain.load() == 0) return;
  queueRequest(ringtoneSamples, ringtoneCount, AudioSource::Ringtone);
}

// Plays the ringtone again and again until cancelSound() or a newer request.
void startRinging() {
  if (!audioReady.load()) { logError("audio", "Speaker is unavailable"); return; }
  if (audioGain.load() == 0) return;
  queueRequest(ringtoneSamples, ringtoneCount, AudioSource::Alarm);
}

void playRecording(const int16_t *samples, uint32_t count) {
  if (!audioReady.load() || audioGain.load() == 0 || !samples || count == 0) return;
  queueRequest(samples, count, AudioSource::Recording);
}

void cancelSound() {
  if (requestMutex) xSemaphoreTake(requestMutex, portMAX_DELAY);
  focus = AudioSource::Ringtone;
  audioCancelled.store(true);
  audioGeneration.fetch_add(1);
  if (audioRequests) xQueueReset(audioRequests);
  if (!workerActive) busy.store(false);
  digitalWrite(PIN_SPEAKER_MUTE, HIGH);
  if (requestMutex) xSemaphoreGive(requestMutex);
}

bool audioBusy() { return busy.load(); }
uint16_t audioPlaybackPeak() { return playbackPeak.load(); }

size_t readMicrophone(int16_t *samples, size_t frames, uint32_t timeoutMs) {
  if (!micAvailable.load()) return 0;
  // The codec's mono ADC is read as a stereo frame; averaging both slots works
  // whether it fills only the left slot or both.
  int16_t stereo[2 * 320];
  size_t done = 0;
  while (done < frames) {
    const size_t chunk = min(frames - done, static_cast<size_t>(320));
    size_t read = 0;
    if (i2s_channel_read(micChannel, stereo, chunk * 2 * sizeof(int16_t), &read, timeoutMs) != ESP_OK) break;
    const size_t got = read / (2 * sizeof(int16_t));
    for (size_t i = 0; i < got; ++i) samples[done + i] = (stereo[2 * i] + stereo[2 * i + 1]) / 2;
    done += got;
    if (got < chunk) break;
  }
  return done;
}

AudioStats audioStats() { return {audioCompleted.load(), audioPeak.load()}; }

uint32_t beginAudioStream() {
  if (!audioReady.load() || !streamBlocks) return 0;
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  if (focus == AudioSource::Alarm) { xSemaphoreGive(requestMutex); return 0; }
  focus = AudioSource::Music;
  const uint32_t token = audioGeneration.fetch_add(1) + 1;
  streamToken.store(token);
  streamFrames.store(0);
  streamPaused.store(false);
  audioCancelled.store(false);
  busy.store(true);
  xQueueReset(streamBlocks);
  const AudioRequest request{token, nullptr, 0, false, AudioSource::Music};
  if (xQueueOverwrite(audioRequests, &request) != pdTRUE) {
    busy.store(false);
    logError("audio", "Unable to queue the music stream");
    xSemaphoreGive(requestMutex);
    return 0;
  }
  xSemaphoreGive(requestMutex);
  return token;
}

bool audioStreamCurrent(uint32_t token) {
  return token != 0 && token == audioGeneration.load() && !audioCancelled.load() && audioReady.load();
}

bool queueAudioStream(uint32_t token, const int16_t *samples, uint16_t count) {
  if (!samples || count == 0 || count > 256 || !audioStreamCurrent(token)) return false;
  StreamBlock block{};
  block.generation = token;
  block.count = count;
  memcpy(block.samples, samples, count * sizeof(int16_t));
  return xQueueSend(streamBlocks, &block, pdMS_TO_TICKS(20)) == pdTRUE && audioStreamCurrent(token);
}

bool finishAudioStream(uint32_t token) {
  if (!audioStreamCurrent(token)) return false;
  const StreamBlock end{token, 0, {}};
  return xQueueSend(streamBlocks, &end, pdMS_TO_TICKS(20)) == pdTRUE;
}

void pauseAudioStream(uint32_t token) {
  if (!requestMutex) return;
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  if (audioStreamCurrent(token)) { streamPaused.store(true); digitalWrite(PIN_SPEAKER_MUTE, HIGH); }
  xSemaphoreGive(requestMutex);
}

void resumeAudioStream(uint32_t token) {
  if (!requestMutex) return;
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  if (audioStreamCurrent(token)) streamPaused.store(false);
  xSemaphoreGive(requestMutex);
}
void stopAudioStream(uint32_t token) {
  if (!requestMutex) return;
  xSemaphoreTake(requestMutex, portMAX_DELAY);
  if (audioStreamCurrent(token)) {
    focus = AudioSource::Ringtone;
    audioCancelled.store(true);
    audioGeneration.fetch_add(1);
    xQueueReset(audioRequests);
    if (!workerActive) busy.store(false);
    digitalWrite(PIN_SPEAKER_MUTE, HIGH);
  }
  xSemaphoreGive(requestMutex);
}
bool audioStreamFinished(uint32_t token) { return token != 0 && streamDone.load() == token; }
uint32_t audioStreamFrames(uint32_t token) { return token == streamToken.load() ? streamFrames.load() : 0; }
