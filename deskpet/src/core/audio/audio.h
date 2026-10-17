#pragma once
#include <stddef.h>
#include <stdint.h>

struct AudioStats {
  uint32_t completed;
  uint32_t peak;
};

// Configures the ES8311 codec over I2S/I2C (Wire must already be running) and
// starts the speaker task. The microphone shares the same I2S port.
bool initAudio();
bool audioIsReady();
bool micReady();
// 0 to 100. Zero also switches the amplifier off.
void setAudioGain(uint8_t gain);
// Plays the alarm ringtone once; used by the test buttons and the microphone test.
// Pre-empts a recording.
void requestSound();
// Plays the alarm ringtone again and again until cancelSound() or a newer request.
void startRinging();
// Plays mono 16 kHz samples at the current volume. The buffer must stay valid
// until audioBusy() turns false.
void playRecording(const int16_t *samples, uint32_t count);
void cancelSound();
// True from the moment a sound is queued until the speaker is off again.
bool audioBusy();
// Peak of the block currently being played, for lip-sync.
uint16_t audioPlaybackPeak();
// Reads up to `frames` mono microphone samples; returns how many were read.
size_t readMicrophone(int16_t *samples, size_t frames, uint32_t timeoutMs);
AudioStats audioStats();
