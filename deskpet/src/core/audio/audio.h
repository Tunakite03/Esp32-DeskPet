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
// Replaces the built-in alarm ringtone (mono 16 kHz). The buffer must stay valid
// for the rest of the run; call it before the first sound.
void setRingtone(const int16_t *samples, uint32_t count);
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

// Buffered mono 16 kHz streaming. The decoder runs on its own worker; the
// existing speaker worker remains the only task writing to I2S. Tokens prevent
// stale music buffers from reaching a newer alarm or recording request.
uint32_t beginAudioStream();
bool queueAudioStream(uint32_t token, const int16_t *samples, uint16_t count);
bool finishAudioStream(uint32_t token);
void pauseAudioStream(uint32_t token);
void resumeAudioStream(uint32_t token);
void stopAudioStream(uint32_t token);
bool audioStreamCurrent(uint32_t token);
bool audioStreamFinished(uint32_t token);
uint32_t audioStreamFrames(uint32_t token);
