#include "src/features/ui/scenes/scenes.h"

#include "src/features/music/music.h"
#include "src/features/ui/ui_draw.h"

namespace ui {
namespace {
enum class TransportIcon : uint8_t { Previous, Play, Pause, Next };

String clippedText(const char *value, const UiFont &font, int width) {
  String label;
  for (const uint8_t *byte = reinterpret_cast<const uint8_t *>(value); *byte; ++byte) {
    if (*byte >= 32 && *byte <= 126) label += static_cast<char>(*byte);
    else if ((*byte & 0xC0) != 0x80) label += '?';
  }
  // Glyph ink can extend one pixel beyond its advance.
  if (textWidth(label, font) < width) return label;
  const int suffixWidth = textWidth("...", font);
  while (label.length() && textWidth(label, font) + suffixWidth >= width) label.remove(label.length() - 1);
  return label + "...";
}

const char *phaseLabel(MusicPhase phase) {
  switch (phase) {
    case MusicPhase::Unavailable: return "Unavailable";
    case MusicPhase::Scanning: return "Scanning";
    case MusicPhase::Stopped: return "Stopped";
    case MusicPhase::Loading: return "Loading";
    case MusicPhase::Playing: return "Playing";
    case MusicPhase::Paused: return "Paused";
    case MusicPhase::Error: return "Error";
  }
  return "Unavailable";
}

const char *errorDetail(MusicError error) {
  switch (error) {
    case MusicError::None: return "";
    case MusicError::NoCard: return "Insert card and restart DeskPet";
    case MusicError::NoAudio: return "Speaker unavailable";
    case MusicError::Memory: return "Not enough memory";
    case MusicError::Directory: return "Cannot read /music";
    case MusicError::Empty: return "Copy MP3 / WAV files to /music";
    case MusicError::Index: return "Select a track from Library";
    case MusicError::Open: return "Cannot open this track";
    case MusicError::Decode: return "MP3 / WAV decoding failed";
    case MusicError::Output: return "Speaker output failed";
    case MusicError::Queue: return "Player busy; try again";
  }
  return "Playback error";
}

void transportButton(const Rect &rect, TransportIcon icon, uint16_t background, uint16_t ink) {
  canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, 12, background);
  canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, 12,
                       background == COLOR_BACKGROUND ? COLOR_OUTLINE : background);
  const int x = rect.x + rect.w / 2, y = rect.y + rect.h / 2;
  switch (icon) {
    case TransportIcon::Previous:
      canvas.fillRect(x - 11, y - 9, 3, 18, ink);
      canvas.fillTriangle(x + 8, y - 10, x - 5, y, x + 8, y + 10, ink);
      break;
    case TransportIcon::Next:
      canvas.fillTriangle(x - 8, y - 10, x + 5, y, x - 8, y + 10, ink);
      canvas.fillRect(x + 8, y - 9, 3, 18, ink);
      break;
    case TransportIcon::Pause:
      canvas.fillRoundRect(x - 10, y - 11, 6, 22, 2, ink);
      canvas.fillRoundRect(x + 4, y - 11, 6, 22, 2, ink);
      break;
    case TransportIcon::Play:
      canvas.fillTriangle(x - 7, y - 12, x - 7, y + 12, x + 11, y, ink);
      break;
  }
}

void drawPlayer(const AppState &state, const MusicStatus &music) {
  header("MUSIC", state.battery);
  MusicTrack track;
  const bool hasTrack = music.count && music.index < music.count && getMusicTrack(music.index, track);
  const char *title = music.phase == MusicPhase::Scanning ? "Scanning music"
                    : music.error == MusicError::NoCard ? "No microSD card"
                    : music.error == MusicError::Empty ? "No music tracks"
                    : !music.ready ? "Music unavailable"
                    : !music.count ? "No music tracks"
                    : hasTrack ? (track.title[0] ? track.title : "Unnamed track")
                    : "Track unavailable";
  uiText(18, 38, clippedText(title, FONT_TITLE, 284), FONT_TITLE, COLOR_TEXT);
  const String position = music.count ? "Track " + String(music.index + 1) + " / " + String(music.count) : "MICROSD /music";
  uiText(18, 64, position, FONT_SMALL, COLOR_TEXT);
  uiRightAligned(64, phaseLabel(music.phase), FONT_SMALL);

  const uint32_t seconds = music.elapsedMs / 1000;
  char elapsed[16];
  snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu", static_cast<unsigned long>(seconds / 60),
           static_cast<unsigned long>(seconds % 60));
  uiText(18, 85, elapsed, FONT_SMALL, COLOR_TEXT);
  if (music.phase == MusicPhase::Scanning) {
    uiText(76, 85, "Finding MP3 / WAV files", FONT_SMALL, COLOR_TEXT);
  } else if (music.error != MusicError::None) {
    uiText(76, 85, clippedText(errorDetail(music.error), FONT_SMALL, 226), FONT_SMALL, COLOR_TEXT);
  } else if (!music.count) {
    uiText(76, 85, "Copy MP3 / WAV files to /music", FONT_SMALL, COLOR_TEXT);
  } else {
    uiRightAligned(85, String(music.progress) + "%", FONT_SMALL);
  }
  constexpr Rect progress{18, 105, 284, 3};
  canvas.fillRoundRect(progress.x, progress.y, progress.w, progress.h, 1, COLOR_OUTLINE);
  const int extent = progress.w * min<uint8_t>(music.progress, 100) / 100;
  if (extent > 0) canvas.fillRoundRect(progress.x, progress.y, extent, progress.h, 1, COLOR_SKY);

  const bool available = music.ready && music.count && music.phase != MusicPhase::Scanning;
  const uint16_t ink = available ? COLOR_TEXT : COLOR_MUTED;
  transportButton(MUSIC_PREVIOUS, TransportIcon::Previous, COLOR_BACKGROUND, ink);
  transportButton(MUSIC_PLAY, music.phase == MusicPhase::Playing ? TransportIcon::Pause : TransportIcon::Play,
                  available ? COLOR_ACCENT : COLOR_SURFACE, ink);
  transportButton(MUSIC_NEXT, TransportIcon::Next, COLOR_BACKGROUND, ink);
  pill(MUSIC_VOLUME_DOWN, "-", COLOR_SURFACE);
  const String volume = state.device.muted ? "Muted" : String(state.device.volume) + "%";
  uiText(77 - textWidth(volume, FONT_SMALL) / 2, 181, volume, FONT_SMALL, COLOR_TEXT);
  pill(MUSIC_VOLUME_UP, "+", COLOR_SURFACE);
  pill(MUSIC_STOP, "Stop", COLOR_SURFACE);
  pill(MUSIC_LIBRARY, "Library", COLOR_SURFACE);
}

void drawLibrary(const AppState &state, const MusicStatus &music) {
  header("MUSIC LIBRARY", state.battery);
  const uint16_t pages = music.count ? (music.count + MUSIC_ROWS_PER_PAGE - 1) / MUSIC_ROWS_PER_PAGE : 1;
  const uint16_t page = min<uint16_t>(state.musicLibraryPage, pages - 1);
  if (music.phase == MusicPhase::Scanning) {
    uiCentered(57, "Scanning /music", FONT_TITLE, COLOR_TEXT);
    uiCentered(91, "Looking for MP3 / WAV tracks", FONT_SMALL, COLOR_TEXT);
  } else if (!music.count) {
    const char *title = music.error == MusicError::NoCard ? "No microSD card"
                      : music.error == MusicError::Empty || music.error == MusicError::None ? "No music tracks"
                      : "Library unavailable";
    uiCentered(57, title, FONT_TITLE, COLOR_TEXT);
    const char *detail = music.error == MusicError::None || music.error == MusicError::Empty
                       ? "Copy MP3 / WAV files to /music" : errorDetail(music.error);
    uiCentered(91, detail, FONT_SMALL, COLOR_TEXT);
    uiCentered(117, "Tap Rescan to refresh", FONT_SMALL, COLOR_TEXT);
  } else {
    for (uint8_t slot = 0; slot < MUSIC_ROWS_PER_PAGE; ++slot) {
      const uint16_t index = page * MUSIC_ROWS_PER_PAGE + slot;
      if (index >= music.count) break;
      const Rect row = musicTrackRow(slot);
      const bool selected = index == music.index;
      canvas.fillRoundRect(row.x, row.y, row.w, row.h, 8, COLOR_SURFACE);
      if (selected) {
        canvas.drawRoundRect(row.x, row.y, row.w, row.h, 8, COLOR_SKY);
        canvas.fillCircle(row.right() - 11, row.y + row.h / 2, 3, COLOR_SKY);
      }
      const String number = String(index + 1);
      uiText(row.x + 11, row.y + 12, number, FONT_SMALL, COLOR_TEXT, COLOR_SURFACE);
      MusicTrack track;
      const char *title = getMusicTrack(index, track) ? (track.title[0] ? track.title : "Unnamed track") : "Track unavailable";
      uiText(row.x + 36, row.y + 9, clippedText(title, FONT_BODY, row.w - 56), FONT_BODY, COLOR_TEXT, COLOR_SURFACE);
    }
  }
  pill(MUSIC_LIST_PREVIOUS, "<", COLOR_SURFACE);
  const String pagination = String(page + 1) + "/" + String(pages);
  uiText(88 - textWidth(pagination, FONT_SMALL) / 2, 181, pagination, FONT_SMALL, COLOR_TEXT);
  pill(MUSIC_LIST_NEXT, ">", COLOR_SURFACE);
  pill(MUSIC_RESCAN, "Rescan", COLOR_SURFACE);
  pill(MUSIC_BACK, "Back", COLOR_SURFACE);
}
}  // namespace

void drawMusic(const AppState &state, const MusicStatus &music) {
  if (state.musicLibraryOpen) drawLibrary(state, music);
  else drawPlayer(state, music);
}
}  // namespace ui
