#include "src/features/music/music.h"

#include <Arduino.h>
#include <AudioFileSourceFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutput.h>
#include <SD_MMC.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <strings.h>
#include "src/core/audio/audio.h"
#include "src/core/storage/sd_card.h"
#include "src/core/system/log.h"

namespace {
enum class Action : uint8_t { Scan, Play, Stop };
struct Command { Action action; uint16_t index; uint32_t epoch; };
QueueHandle_t commands = nullptr;
portMUX_TYPE stateMutex = portMUX_INITIALIZER_UNLOCKED;
MusicStatus state;
MusicTrack *tracks = nullptr;
uint8_t *decoderMemory = nullptr;
std::atomic<uint32_t> commandEpoch{0}, activeToken{0};

void setPhase(uint32_t epoch, MusicPhase phase, MusicError error) {
  portENTER_CRITICAL(&stateMutex);
  if (epoch == commandEpoch.load()) { state.phase = phase; state.error = error; ++state.revision; }
  portEXIT_CRITICAL(&stateMutex);
  if (error != MusicError::None) logError("music", musicErrorName(error));
}

void sendCommand(Action action, uint16_t index) {
  stopAudioStream(activeToken.load());
  const uint32_t epoch = commandEpoch.fetch_add(1) + 1;
  portENTER_CRITICAL(&stateMutex);
  state.phase = action == Action::Scan ? MusicPhase::Scanning : action == Action::Play ? MusicPhase::Loading : MusicPhase::Stopped;
  state.error = MusicError::None;
  if (action == Action::Play) state.index = index;
  state.elapsedMs = 0;
  state.progress = 0;
  ++state.revision;
  portEXIT_CRITICAL(&stateMutex);
  const Command command{action, index, epoch};
  if (!commands || xQueueOverwrite(commands, &command) != pdTRUE) setPhase(epoch, MusicPhase::Error, MusicError::Queue);
}

bool supportedFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3") || lower.endsWith(".wav");
}

void scanLibrary(uint32_t epoch) {
  portENTER_CRITICAL(&stateMutex);
  state.count = 0;
  portEXIT_CRITICAL(&stateMutex);
  if (!SD_MMC.exists("/music") && !SD_MMC.mkdir("/music")) {
    setPhase(epoch, MusicPhase::Error, MusicError::Directory);
    return;
  }
  File directory = SD_MMC.open("/music", FILE_READ);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    setPhase(epoch, MusicPhase::Error, MusicError::Directory);
    return;
  }
  uint16_t count = 0;
  for (uint16_t inspected = 0; inspected < 1024 && count < MUSIC_MAX_TRACKS && epoch == commandEpoch.load(); ++inspected) {
    File entry = directory.openNextFile(FILE_READ);
    if (!entry) break;
    const String name = entry.name();
    const String path = entry.path();
    if (!entry.isDirectory() && !name.startsWith(".") && supportedFile(name) && path.length() < sizeof(MusicTrack::path)) {
      MusicTrack &track = tracks[count++];
      snprintf(track.path, sizeof(track.path), "%s", path.c_str());
      String title = name.substring(0, name.lastIndexOf('.'));
      // The built-in fonts are ASCII. Keep the UTF-8 path intact for file access.
      for (size_t index = 0; index < title.length(); ++index) {
        if (static_cast<uint8_t>(title[index]) >= 128) title.setCharAt(index, '?');
      }
      snprintf(track.title, sizeof(track.title), "%s", title.c_str());
    }
    entry.close();
    vTaskDelay(1);
  }
  directory.close();
  if (epoch != commandEpoch.load()) return;
  std::sort(tracks, tracks + count, [](const MusicTrack &left, const MusicTrack &right) {
    return strcasecmp(left.title, right.title) < 0;
  });
  portENTER_CRITICAL(&stateMutex);
  state.count = count;
  state.index = 0;
  portEXIT_CRITICAL(&stateMutex);
  setPhase(epoch, MusicPhase::Stopped, MusicError::None);
}

// Adapter from the decoder library to the existing mono 16 kHz speaker queue.
// Rate conversion averages the input frames within each output interval.
class MusicOutput final : public AudioOutput {
 public:
  explicit MusicOutput(uint32_t token) : token_(token) { hertz = 16000; channels = 2; }
  bool SetRate(int rate) override {
    if (rate < 8000 || rate > 48000) return false;
    if (hertz != rate) { phase_ = 0; sum_ = 0; inputs_ = 0; }
    hertz = static_cast<uint16_t>(rate);
    return true;
  }
  bool SetChannels(int count) override {
    if (count != 1 && count != 2) return false;
    channels = static_cast<uint8_t>(count);
    return true;
  }
  bool begin() override { return audioStreamCurrent(token_); }
  bool ConsumeSample(int16_t sample[2]) override {
    if (!audioStreamCurrent(token_)) return false;
    // ESP8266Audio 2.4.1 offers its zero-initialized lastSample before the
    // first decode. It is a retry sentinel, not evidence of valid audio.
    if (initialSample_) { initialSample_ = false; return true; }
    // Yield to the music worker once per output block, even when the speaker can
    // immediately accept more data, so controls never wait for the end of a song.
    if (used_ >= 254) { flushBlock(); return false; }
    sum_ += channels == 1 ? sample[0] : (static_cast<int32_t>(sample[0]) + sample[1]) / 2;
    ++inputs_;
    phase_ += 16000;
    while (phase_ >= hertz) {
      const int16_t value = inputs_ == 0 ? last_ : static_cast<int16_t>(sum_ / inputs_);
      buffer_[used_++] = value;
      last_ = value;
      ++generated_;
      phase_ -= hertz;
      sum_ = 0;
      inputs_ = 0;
    }
    return true;
  }
  bool stop() override {
    while (used_ != 0 && audioStreamCurrent(token_)) {
      if (!flushBlock()) vTaskDelay(1);
    }
    return true;
  }
  uint32_t generated() const { return generated_; }
 private:
  bool flushBlock() {
    if (used_ == 0) return true;
    if (!queueAudioStream(token_, buffer_, used_)) return false;
    used_ = 0;
    return true;
  }
  uint32_t token_, phase_ = 0, generated_ = 0;
  bool initialSample_ = true;
  int32_t sum_ = 0;
  uint16_t inputs_ = 0, used_ = 0;
  int16_t last_ = 0, buffer_[256]{};
};

class MusicFileSource final : public AudioFileSourceFS {
 public:
  MusicFileSource() : AudioFileSourceFS(SD_MMC) {}
  void setRequest(uint32_t epoch) { epoch_ = epoch; token_ = 0; }
  void setToken(uint32_t token) { token_ = token; }
  bool open(const char *path) override {
    failed_ = false;
    position_ = 0;
    const bool opened = AudioFileSourceFS::open(path);
    size_ = opened ? AudioFileSourceFS::getSize() : 0;
    if (opened && size_ >= 10) {
      uint8_t header[10];
      const uint32_t received = read(header, sizeof(header));
      if (received != sizeof(header)) { close(); return false; }
      uint32_t start = 0;
      if (memcmp(header, "ID3", 3) == 0) {
        if (header[3] < 2 || header[3] > 4 || ((header[6] | header[7] | header[8] | header[9]) & 0x80)) {
          failed_ = true; close(); return false;
        }
        const uint32_t bytes = static_cast<uint32_t>(header[6]) << 21 | static_cast<uint32_t>(header[7]) << 14 |
                                static_cast<uint32_t>(header[8]) << 7 | header[9];
        start = 10 + bytes + (header[3] == 4 && (header[5] & 0x10) ? 10 : 0);
        if (start >= size_) { failed_ = true; close(); return false; }
      }
      if (!seek(static_cast<int32_t>(start), SEEK_SET)) { failed_ = true; close(); return false; }
    }
    return opened;
  }
  uint32_t read(void *data, uint32_t length) override {
    if (epoch_ != commandEpoch.load() || (token_ != 0 && !audioStreamCurrent(token_))) return 0;
    const uint32_t received = AudioFileSourceFS::read(data, length);
    position_ += received;
    if (received < length && position_ < size_) failed_ = true;
    return received;
  }
  bool seek(int32_t position, int direction) override {
    if (epoch_ != commandEpoch.load() || (token_ != 0 && !audioStreamCurrent(token_))) return false;
    const bool moved = AudioFileSourceFS::seek(position, direction);
    if (moved) position_ = AudioFileSourceFS::getPos();
    return moved;
  }
  uint32_t getSize() override { return size_; }
  uint32_t getPos() override { return position_; }
  bool failed() const { return failed_; }
 private:
  uint32_t position_ = 0, size_ = 0;
  uint32_t epoch_ = 0, token_ = 0;
  bool failed_ = false;
};

bool validWav(MusicFileSource &source) {
  uint8_t header[12];
  if (source.read(header, sizeof(header)) != sizeof(header) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) return false;
  bool formatValid = false;
  uint16_t frameSize = 0;
  for (uint8_t chunk = 0; chunk < 32; ++chunk) {
    uint8_t info[8];
    if (source.read(info, sizeof(info)) != sizeof(info)) return false;
    const uint32_t size = static_cast<uint32_t>(info[4]) | static_cast<uint32_t>(info[5]) << 8 |
                          static_cast<uint32_t>(info[6]) << 16 | static_cast<uint32_t>(info[7]) << 24;
    if (source.getPos() > source.getSize() || size > source.getSize() - source.getPos()) return false;
    if (memcmp(info, "fmt ", 4) == 0) {
      uint8_t format[16];
      if (size < sizeof(format) || source.read(format, sizeof(format)) != sizeof(format)) return false;
      const uint16_t encoding = format[0] | format[1] << 8;
      const uint16_t channelCount = format[2] | format[3] << 8;
      const uint32_t rate = static_cast<uint32_t>(format[4]) | static_cast<uint32_t>(format[5]) << 8 |
                            static_cast<uint32_t>(format[6]) << 16 | static_cast<uint32_t>(format[7]) << 24;
      const uint32_t byteRate = static_cast<uint32_t>(format[8]) | static_cast<uint32_t>(format[9]) << 8 |
                                static_cast<uint32_t>(format[10]) << 16 | static_cast<uint32_t>(format[11]) << 24;
      const uint16_t alignment = format[12] | format[13] << 8;
      const uint16_t bits = format[14] | format[15] << 8;
      formatValid = encoding == 1 && (channelCount == 1 || channelCount == 2) && bits == 16 && rate >= 8000 && rate <= 48000 &&
                    alignment == channelCount * 2 && byteRate == rate * alignment;
      if (!formatValid) return false;
      frameSize = alignment;
      const uint32_t remaining = size - sizeof(format) + (size & 1);
      if (remaining > INT32_MAX || !source.seek(static_cast<int32_t>(remaining), SEEK_CUR)) return false;
      continue;
    }
    if (memcmp(info, "data", 4) == 0) return formatValid && size >= frameSize && size % frameSize == 0 && source.seek(0, SEEK_SET);
    if (size > INT32_MAX - 1 || !source.seek(static_cast<int32_t>(size + (size & 1)), SEEK_CUR)) return false;
  }
  return false;
}

void musicWorker(void *) {
  MusicFileSource source;
  std::unique_ptr<AudioGenerator> decoder;
  std::unique_ptr<MusicOutput> output;
  uint32_t epoch = 0, token = 0, fileSize = 0;
  bool draining = false, autoNext = false;
  uint16_t nextIndex = 0;
  for (;;) {
    Command command{};
    bool received = xQueueReceive(commands, &command, decoder ? 0 : pdMS_TO_TICKS(20)) == pdTRUE;
    if (!received && autoNext) { command = {Action::Play, nextIndex, commandEpoch.load()}; received = true; }
    if (received) {
      autoNext = false;
      stopAudioStream(token);
      if (decoder) decoder->stop();
      decoder.reset();
      output.reset();
      source.close();
      draining = false;
      token = 0;
      activeToken.store(0);
      epoch = command.epoch;
      if (epoch != commandEpoch.load()) continue;
      if (command.action == Action::Stop) continue;
      if (command.action == Action::Scan) { scanLibrary(epoch); continue; }
      MusicTrack track;
      if (!getMusicTrack(command.index, track)) { setPhase(epoch, MusicPhase::Error, MusicError::Index); continue; }
      portENTER_CRITICAL(&stateMutex);
      state.index = command.index;
      state.elapsedMs = 0;
      state.progress = 0;
      portEXIT_CRITICAL(&stateMutex);
      source.setRequest(epoch);
      if (!source.open(track.path)) { setPhase(epoch, MusicPhase::Error, source.failed() ? MusicError::Decode : MusicError::Open); continue; }
      String name = track.path;
      name.toLowerCase();
      const bool wav = name.endsWith(".wav");
      if (wav && !validWav(source)) { source.close(); setPhase(epoch, MusicPhase::Error, MusicError::Decode); continue; }
      fileSize = source.getSize();
      token = beginAudioStream();
      activeToken.store(token);
      source.setToken(token);
      if (token == 0) { source.close(); setPhase(epoch, MusicPhase::Error, MusicError::Output); continue; }
      output.reset(new (std::nothrow) MusicOutput(token));
      if (wav) decoder.reset(new (std::nothrow) AudioGeneratorWAV());
      else decoder.reset(new (std::nothrow) AudioGeneratorMP3(decoderMemory, AudioGeneratorMP3::preAllocSize()));
      if (!output || !decoder || !decoder->begin(&source, output.get())) {
        stopAudioStream(token);
        decoder.reset(); output.reset(); source.close();
        setPhase(epoch, MusicPhase::Error, MusicError::Decode);
        continue;
      }
      setPhase(epoch, MusicPhase::Playing, MusicError::None);
    }
    if (!decoder) continue;
    if (epoch != commandEpoch.load()) { vTaskDelay(1); continue; }
    if (!audioStreamCurrent(token)) {
      decoder->stop(); decoder.reset(); output.reset(); source.close();
      setPhase(epoch, audioIsReady() ? MusicPhase::Stopped : MusicPhase::Error,
               audioIsReady() ? MusicError::None : MusicError::Output);
      continue;
    }
    if (musicStatus().phase == MusicPhase::Paused) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    if (!draining) {
      const bool running = decoder->loop();
      if (epoch != commandEpoch.load() || !audioStreamCurrent(token)) continue;
      const uint8_t progress = fileSize == 0 ? 0 : static_cast<uint8_t>(min<uint64_t>(100, static_cast<uint64_t>(source.getPos()) * 100 / fileSize));
      portENTER_CRITICAL(&stateMutex);
      state.progress = progress;
      portEXIT_CRITICAL(&stateMutex);
      if (!running) {
        output->stop();
        if (output->generated() == 0 || source.failed()) {
          stopAudioStream(token);
          decoder->stop(); decoder.reset(); output.reset(); source.close();
          setPhase(epoch, MusicPhase::Error, MusicError::Decode);
          continue;
        }
        draining = finishAudioStream(token);
      }
    } else if (audioStreamFinished(token)) {
      const uint32_t elapsed = audioStreamFrames(token) / 16;
      decoder->stop(); decoder.reset(); output.reset(); source.close();
      portENTER_CRITICAL(&stateMutex);
      state.elapsedMs = elapsed;
      state.progress = 100;
      nextIndex = state.index + 1;
      autoNext = nextIndex < state.count;
      portEXIT_CRITICAL(&stateMutex);
      setPhase(epoch, MusicPhase::Stopped, MusicError::None);
    }
    vTaskDelay(1);
  }
}
}  // namespace

const char *musicPhaseName(MusicPhase phase) {
  switch (phase) {
    case MusicPhase::Unavailable: return "unavailable";
    case MusicPhase::Scanning: return "scanning";
    case MusicPhase::Stopped: return "stopped";
    case MusicPhase::Loading: return "loading";
    case MusicPhase::Playing: return "playing";
    case MusicPhase::Paused: return "paused";
    case MusicPhase::Error: return "error";
  }
  return "invalid";
}

const char *musicErrorName(MusicError error) {
  switch (error) {
    case MusicError::None: return "none";
    case MusicError::NoCard: return "Insert an SD card and restart";
    case MusicError::NoAudio: return "Speaker is unavailable";
    case MusicError::Memory: return "Music buffer allocation failed";
    case MusicError::Directory: return "Cannot open /music on the SD card";
    case MusicError::Empty: return "Copy MP3 or 16-bit WAV files to /music";
    case MusicError::Index: return "Track index is outside the current library";
    case MusicError::Open: return "Cannot open the selected track";
    case MusicError::Decode: return "Invalid or unsupported MP3/WAV audio";
    case MusicError::Output: return "Audio stream is unavailable";
    case MusicError::Queue: return "Music command could not be queued";
  }
  return "Invalid music error code";
}

MusicStatus musicStatus() {
  portENTER_CRITICAL(&stateMutex);
  MusicStatus snapshot = state;
  portEXIT_CRITICAL(&stateMutex);
  if (snapshot.phase == MusicPhase::Playing || snapshot.phase == MusicPhase::Paused) {
    snapshot.elapsedMs = audioStreamFrames(activeToken.load()) / 16;
    snapshot.peak = audioPlaybackPeak();
  }
  return snapshot;
}

bool getMusicTrack(uint16_t index, MusicTrack &track) {
  portENTER_CRITICAL(&stateMutex);
  const bool valid = tracks && index < state.count && state.phase != MusicPhase::Scanning;
  if (valid) track = tracks[index];
  portEXIT_CRITICAL(&stateMutex);
  return valid;
}

bool initMusic() {
  if (!sdCardStatus().mounted || !audioIsReady()) {
    setPhase(commandEpoch.load(), MusicPhase::Unavailable, !sdCardStatus().mounted ? MusicError::NoCard : MusicError::NoAudio);
    return false;
  }
  tracks = static_cast<MusicTrack *>(ps_calloc(MUSIC_MAX_TRACKS, sizeof(MusicTrack)));
  decoderMemory = static_cast<uint8_t *>(ps_malloc(AudioGeneratorMP3::preAllocSize()));
  commands = xQueueCreate(1, sizeof(Command));
  if (!tracks || !decoderMemory || !commands || xTaskCreatePinnedToCore(musicWorker, "music", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
    if (commands) { vQueueDelete(commands); commands = nullptr; }
    free(tracks); tracks = nullptr;
    free(decoderMemory); decoderMemory = nullptr;
    setPhase(commandEpoch.load(), MusicPhase::Unavailable, MusicError::Memory);
    return false;
  }
  portENTER_CRITICAL(&stateMutex);
  state.ready = true;
  portEXIT_CRITICAL(&stateMutex);
  scanMusic();
  return true;
}

bool musicActive() {
  const MusicPhase phase = musicStatus().phase;
  return phase == MusicPhase::Playing || phase == MusicPhase::Paused || phase == MusicPhase::Loading;
}

void scanMusic() { if (musicStatus().ready) sendCommand(Action::Scan, 0); }
void selectMusicTrack(uint16_t index) {
  const MusicStatus snapshot = musicStatus();
  if (!snapshot.ready || snapshot.phase == MusicPhase::Scanning) return;
  if (index >= snapshot.count) {
    sendCommand(Action::Stop, snapshot.index);
    setPhase(commandEpoch.load(), MusicPhase::Error, snapshot.count == 0 ? MusicError::Empty : MusicError::Index);
    return;
  }
  sendCommand(Action::Play, index);
}
void playMusic() { if (musicStatus().phase == MusicPhase::Paused) resumeMusic(); else selectMusicTrack(musicStatus().index); }
void pauseMusic() {
  if (musicStatus().phase != MusicPhase::Playing) return;
  pauseAudioStream(activeToken.load());
  setPhase(commandEpoch.load(), MusicPhase::Paused, MusicError::None);
}
void resumeMusic() {
  if (musicStatus().phase != MusicPhase::Paused) return;
  resumeAudioStream(activeToken.load());
  setPhase(commandEpoch.load(), MusicPhase::Playing, MusicError::None);
}
void stopMusic() { if (musicStatus().ready) sendCommand(Action::Stop, musicStatus().index); }
void nextMusic() {
  const MusicStatus snapshot = musicStatus();
  if (snapshot.count != 0) selectMusicTrack((snapshot.index + 1) % snapshot.count);
}
void previousMusic() {
  const MusicStatus snapshot = musicStatus();
  if (snapshot.count != 0) selectMusicTrack((snapshot.index + snapshot.count - 1) % snapshot.count);
}
