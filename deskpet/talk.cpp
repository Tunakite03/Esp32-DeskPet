#include "talk.h"

#include <Arduino.h>
#include <atomic>
#include <math.h>
#include "audio.h"
#include "log.h"

namespace {
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t BLOCK = 320;                    // 20 ms of audio
constexpr size_t MAX_SAMPLES = SAMPLE_RATE * 6;  // longest phrase that is repeated
constexpr size_t PREROLL = SAMPLE_RATE / 4;      // kept from just before speech was detected
constexpr uint8_t START_BLOCKS = 3;              // 60 ms above the threshold starts a phrase
constexpr uint8_t END_BLOCKS = 35;               // 700 ms of quiet ends it
constexpr uint8_t TRAILING_BLOCKS = 8;           // quiet kept at the end of a phrase
constexpr uint16_t MIN_VOICED_BLOCKS = 12;       // claps and desk bumps are ignored
constexpr uint32_t COOLDOWN = 400;               // ignore the speaker's own echo
constexpr float PITCH_RATIO = 1.6F;
constexpr size_t GRAIN = 1024, HOP = GRAIN / 2;

int16_t *recording = nullptr;
int16_t *voice = nullptr;
float *mix = nullptr;
float window[GRAIN];
int16_t preroll[PREROLL];
size_t prerollHead = 0;

std::atomic<bool> active{false};
std::atomic<bool> testRunning{false};
std::atomic<uint8_t> phase{static_cast<uint8_t>(TalkPhase::Off)};
std::atomic<uint8_t> level{0};
std::atomic<uint32_t> phrases{0};
std::atomic<uint32_t> lastRms{0}, peakRms{0}, noiseFloorRms{0}, lastPhraseSamples{0};
// Detection is relative to the room's noise floor; these are only lower bounds
// (block RMS) for a very quiet room. Adjustable at runtime with "talk-tune".
std::atomic<uint16_t> minStartRms{90}, minEndRms{55};
constexpr float START_ABOVE_FLOOR = 3.0F, END_ABOVE_FLOOR = 1.8F;

void setPhase(TalkPhase next) { phase.store(static_cast<uint8_t>(next)); }

uint32_t blockRms(const int16_t *samples, size_t count) {
  int64_t sum = 0, squares = 0;
  for (size_t i = 0; i < count; ++i) {
    sum += samples[i];
    squares += static_cast<int32_t>(samples[i]) * samples[i];
  }
  const float mean = static_cast<float>(sum) / count;
  return static_cast<uint32_t>(sqrtf(max(0.0F, static_cast<float>(squares) / count - mean * mean)));
}

void rememberPreroll(const int16_t *samples) {
  for (size_t i = 0; i < BLOCK; ++i) {
    preroll[prerollHead] = samples[i];
    prerollHead = (prerollHead + 1) % PREROLL;
  }
}

size_t copyPreroll() {
  const size_t tail = PREROLL - prerollHead;
  memcpy(recording, preroll + prerollHead, tail * sizeof(int16_t));
  memcpy(recording + tail, preroll, prerollHead * sizeof(int16_t));
  return PREROLL;
}

// Raises the pitch without changing the length: Hann-windowed grains are laid
// down every HOP samples, each one read PITCH_RATIO times faster than real time.
void makeVoice(size_t count) {
  memset(mix, 0, count * sizeof(float));
  for (size_t start = 0; start < count; start += HOP) {
    for (size_t j = 0; j < GRAIN && start + j < count; ++j) {
      const float source = start + j * PITCH_RATIO;
      const size_t index = static_cast<size_t>(source);
      if (index + 1 >= count) break;
      const float fraction = source - index;
      const float sample = recording[index] + (recording[index + 1] - recording[index]) * fraction;
      mix[start + j] += sample * window[j];
    }
  }
  float peak = 1;
  for (size_t i = 0; i < count; ++i) peak = max(peak, fabsf(mix[i]));
  // Quiet speech is brought up to a comfortable level, but noise is not boosted endlessly.
  const float gain = min(8.0F, 20000.0F / peak);
  for (size_t i = 0; i < count; ++i) voice[i] = static_cast<int16_t>(mix[i] * gain);
}

uint8_t loudness(uint32_t rms, float startThreshold, float endThreshold) {
  return rms > startThreshold * 3 ? 3 : rms > startThreshold * 1.5F ? 2 : rms > endThreshold ? 1 : 0;
}

void talkWorker(void *) {
  int16_t block[BLOCK];
  float noiseFloor = 40;
  size_t length = 0;
  uint8_t loudBlocks = 0, quietBlocks = 0;
  uint16_t voicedBlocks = 0;
  uint32_t cooldownUntil = 0;
  bool hearing = false, speaking = false;
  for (;;) {
    if (!active.load() || testRunning.load()) {
      if (!active.load()) { setPhase(TalkPhase::Off); level.store(0); }
      hearing = false; speaking = false; loudBlocks = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (audioBusy()) {
      // Never listen while the speaker plays, or the pet would repeat itself forever.
      if (readMicrophone(block, BLOCK, 50) != BLOCK) vTaskDelay(pdMS_TO_TICKS(20));
      level.store(speaking ? min<uint32_t>(3, audioPlaybackPeak() / 5000) : 0);
      setPhase(speaking ? TalkPhase::Talking : TalkPhase::Listening);
      hearing = false; loudBlocks = 0;
      cooldownUntil = millis() + COOLDOWN;
      continue;
    }
    speaking = false;
    // A failed read can return at once; sleep so this task never spins on core 0.
    if (readMicrophone(block, BLOCK, 100) != BLOCK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    if (static_cast<int32_t>(millis() - cooldownUntil) < 0) { setPhase(TalkPhase::Listening); continue; }
    const uint32_t rms = blockRms(block, BLOCK);
    const float startThreshold = max(noiseFloor * START_ABOVE_FLOOR, static_cast<float>(minStartRms.load()));
    const float endThreshold = max(noiseFloor * END_ABOVE_FLOOR, static_cast<float>(minEndRms.load()));
    lastRms.store(rms);
    if (rms > peakRms.load()) peakRms.store(rms);
    noiseFloorRms.store(static_cast<uint32_t>(noiseFloor));
    level.store(loudness(rms, startThreshold, endThreshold));

    if (!hearing) {
      rememberPreroll(block);
      loudBlocks = rms > startThreshold ? loudBlocks + 1 : 0;
      if (rms < startThreshold) noiseFloor += (rms - noiseFloor) * (rms < noiseFloor ? 0.2F : 0.02F);
      if (loudBlocks < START_BLOCKS) { setPhase(TalkPhase::Listening); continue; }
      hearing = true;
      length = copyPreroll();
      quietBlocks = 0;
      voicedBlocks = loudBlocks;
      setPhase(TalkPhase::Hearing);
      continue;
    }

    memcpy(recording + length, block, sizeof(block));
    length += BLOCK;
    if (rms > endThreshold) { ++voicedBlocks; quietBlocks = 0; } else { ++quietBlocks; }
    if (quietBlocks < END_BLOCKS && length + BLOCK <= MAX_SAMPLES) continue;

    hearing = false;
    loudBlocks = 0;
    if (voicedBlocks < MIN_VOICED_BLOCKS) { setPhase(TalkPhase::Listening); continue; }
    const size_t trimmed = quietBlocks > TRAILING_BLOCKS ? (quietBlocks - TRAILING_BLOCKS) * BLOCK : 0;
    const size_t count = length - trimmed;
    makeVoice(count);
    lastPhraseSamples.store(count);
    phrases.fetch_add(1);
    speaking = true;
    setPhase(TalkPhase::Talking);
    playRecording(voice, count);
    Serial.printf("{\"event\":\"talk_phrase\",\"samples\":%u}\n", static_cast<unsigned>(count));
  }
}

struct MicLevels { uint32_t average, peak; };

MicLevels measureMic(uint32_t duration) {
  int16_t block[BLOCK];
  uint64_t total = 0;
  uint32_t blocks = 0, peak = 0;
  const uint32_t started = millis();
  while (millis() - started < duration) {
    if (readMicrophone(block, BLOCK, 100) != BLOCK) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    const uint32_t rms = blockRms(block, BLOCK);
    total += rms; ++blocks; peak = max(peak, rms);
  }
  return {blocks ? static_cast<uint32_t>(total / blocks) : 0, peak};
}
}  // namespace

bool initTalk() {
  if (!micReady()) { logError("talk", "Microphone unavailable; talk mode disabled"); return false; }
  recording = static_cast<int16_t *>(ps_malloc(MAX_SAMPLES * sizeof(int16_t)));
  voice = static_cast<int16_t *>(ps_malloc(MAX_SAMPLES * sizeof(int16_t)));
  mix = static_cast<float *>(ps_malloc(MAX_SAMPLES * sizeof(float)));
  if (!recording || !voice || !mix) { logError("talk", "Recording buffer allocation failed"); return false; }
  for (size_t i = 0; i < GRAIN; ++i) window[i] = 0.5F - 0.5F * cosf(6.2831853F * i / GRAIN);
  if (xTaskCreatePinnedToCore(talkWorker, "talk", 6144, nullptr, 1, nullptr, 0) != pdPASS) {
    logError("talk", "Talk worker allocation failed");
    return false;
  }
  return true;
}

void setTalkActive(bool enabled) { active.store(enabled && recording != nullptr); }
TalkPhase talkPhase() { return static_cast<TalkPhase>(phase.load()); }
uint8_t talkLevel() { return level.load(); }
uint32_t talkPhrases() { return phrases.load(); }

void tuneTalk(uint16_t startRms, uint16_t endRms) {
  minStartRms.store(startRms);
  minEndRms.store(endRms);
}

// "peak_rms" is the loudest block since the previous status, so polling it while
// someone speaks shows how far above the thresholds their voice reaches.
void printTalkStatus() {
  Serial.printf("{\"talk_active\":%s,\"phase\":%u,\"level\":%u,\"rms\":%lu,\"peak_rms\":%lu,\"noise_floor\":%lu,"
                "\"min_start\":%u,\"min_end\":%u,\"phrases\":%lu,\"last_samples\":%lu}\n",
                active.load() ? "true" : "false", phase.load(), level.load(),
                static_cast<unsigned long>(lastRms.load()), static_cast<unsigned long>(peakRms.exchange(0)),
                static_cast<unsigned long>(noiseFloorRms.load()), minStartRms.load(), minEndRms.load(),
                static_cast<unsigned long>(phrases.load()), static_cast<unsigned long>(lastPhraseSamples.load()));
}

void runMicTest() {
  if (!micReady()) { logError("talk", "Microphone unavailable"); return; }
  if (active.load()) { logError("talk", "Leave the pet page or turn talk mode off before mic-test"); return; }
  testRunning.store(true);
  measureMic(200);  // drop whatever was left in the DMA buffers
  const MicLevels quiet = measureMic(1000);
  requestSound();
  const MicLevels chime = measureMic(1000);
  testRunning.store(false);
  const bool heardChime = chime.peak > quiet.peak * 3 && chime.peak > 300;
  Serial.printf("{\"mic_test\":\"%s\",\"quiet_rms\":%lu,\"quiet_peak\":%lu,\"chime_rms\":%lu,\"chime_peak\":%lu}\n",
                heardChime ? "pass" : "fail", static_cast<unsigned long>(quiet.average),
                static_cast<unsigned long>(quiet.peak), static_cast<unsigned long>(chime.average),
                static_cast<unsigned long>(chime.peak));
}
