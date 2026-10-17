#pragma once
#include <stdint.h>

// Talk mode: the companion listens through the microphone, records what it
// hears and repeats it in a higher voice. Audio stays in PSRAM and is
// overwritten by the next phrase; nothing is stored or sent anywhere.
enum class TalkPhase : uint8_t { Off, Listening, Hearing, Talking };

// Allocates the recording buffers and starts the listening task (after initAudio).
bool initTalk();
// The microphone is only read while this is true (talk mode on and the pet visible).
void setTalkActive(bool active);
TalkPhase talkPhase();
// 0..3 loudness: the microphone while listening, the voice while talking.
uint8_t talkLevel();
// Phrases heard and repeated since boot, for the render stamp and QA.
uint32_t talkPhrases();
void printTalkStatus();
// Lower bounds (block RMS) for starting and ending a phrase in a very quiet room.
void tuneTalk(uint16_t startRms, uint16_t endRms);
// Records one second of silence and one second over the test chime, then
// prints the microphone levels (serial "mic-test"). Talk mode must be inactive.
void runMicTest();
