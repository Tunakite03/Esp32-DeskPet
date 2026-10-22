#pragma once
#include <stdint.h>

constexpr uint16_t MUSIC_MAX_TRACKS = 64;
enum class MusicPhase : uint8_t { Unavailable, Scanning, Stopped, Loading, Playing, Paused, Error };
enum class MusicError : uint8_t { None, NoCard, NoAudio, Memory, Directory, Empty, Index, Open, Decode, Output, Queue };

struct MusicTrack {
  char path[192]{};
  char title[80]{};
};

struct MusicStatus {
  bool ready = false;
  MusicPhase phase = MusicPhase::Unavailable;
  MusicError error = MusicError::None;
  uint16_t count = 0;
  uint16_t index = 0;
  uint32_t elapsedMs = 0;
  uint8_t progress = 0;
  uint16_t peak = 0;
  uint32_t revision = 0;
};

bool initMusic();
MusicStatus musicStatus();
bool getMusicTrack(uint16_t index, MusicTrack &track);
const char *musicPhaseName(MusicPhase phase);
const char *musicErrorName(MusicError error);
bool musicActive();
void scanMusic();
void selectMusicTrack(uint16_t index);
void playMusic();
void pauseMusic();
void resumeMusic();
void stopMusic();
void nextMusic();
void previousMusic();
