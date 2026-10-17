#include "src/features/ui/ui_draw.h"

#include <WiFi.h>
#include "src/app/state.h"

namespace ui {
namespace {
const UiGlyph *findGlyph(const UiFont &font, char character) {
  // Full fonts hold printable ASCII in order, so the glyph can be indexed directly.
  constexpr uint8_t FIRST = 32, PRINTABLE = 95;
  const uint8_t code = static_cast<uint8_t>(character);
  if (font.count == PRINTABLE) {
    if (code < FIRST || code >= FIRST + PRINTABLE) return nullptr;
    const UiGlyph *glyph = &font.glyphs[code - FIRST];
    if (glyph->code == code) return glyph;
  }
  for (uint8_t i = 0; i < font.count; ++i) {
    if (font.glyphs[i].code == static_cast<uint8_t>(character)) return &font.glyphs[i];
  }
  return nullptr;
}

constexpr const char *NAV_LABELS[PAGE_COUNT] = {"Pet", "Clock", "Weather", "Settings"};
constexpr int NAV_PILL_PADDING = 8, NAV_PILL_Y = 211, NAV_PILL_HEIGHT = 26;

// Icon, 6 px gap and label of one tab.
int navContentWidth(uint8_t page) { return textWidth(NAV_LABELS[page], FONT_SMALL) + 20; }

// Mixes fg over bg; alpha runs from 0 (bg) to 15 (fg).
uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  const int red = (((fg >> 11) & 31) * alpha + ((bg >> 11) & 31) * (15 - alpha)) / 15;
  const int green = (((fg >> 5) & 63) * alpha + ((bg >> 5) & 63) * (15 - alpha)) / 15;
  const int blue = ((fg & 31) * alpha + (bg & 31) * (15 - alpha)) / 15;
  return (red << 11) | (green << 5) | blue;
}

// How much of the pixel at (px, py) the pill covers, 0-15, so its rounded ends are anti-aliased.
uint8_t pillCoverage(const Rect &pill, int px, int py) {
  const float radius = pill.h / 2.0f;
  const float left = pill.x + radius, right = pill.right() - radius;
  const float x = px + 0.5f, y = py + 0.5f;
  const float dx = x < left ? left - x : x > right ? x - right : 0;
  const float dy = y - (pill.y + radius);
  const float edge = radius + 0.5f - sqrtf(dx * dx + dy * dy);
  return edge <= 0 ? 0 : edge >= 1 ? 15 : static_cast<uint8_t>(edge * 15 + 0.5f);
}

// Tab icons and labels are white where the pill lies behind them and muted elsewhere,
// so a label the sliding pill only partly covers splits cleanly at its edge.
uint16_t navInk(const Rect &pill, int px, int py) { return pillCoverage(pill, px, py) >= 8 ? COLOR_TEXT : COLOR_MUTED; }

void fillPill(const Rect &pill, uint16_t color) {
  uint16_t *pixels = canvas.getFramebuffer();
  for (int py = max(pill.y, 0); py < min(pill.bottom(), SCREEN_HEIGHT); ++py) {
    for (int px = max(pill.x, 0); px < min(pill.right(), SCREEN_WIDTH); ++px) {
      const uint8_t coverage = pillCoverage(pill, px, py);
      if (coverage) pixels[py * SCREEN_WIDTH + px] = blend565(color, pixels[py * SCREEN_WIDTH + px], coverage);
    }
  }
}

// Like uiText(), but each glyph pixel takes its colour from navInk() and blends with
// whatever is already on the canvas (the pill or the background).
void navLabel(int x, int y, const char *label, const Rect &pill) {
  uint16_t *pixels = canvas.getFramebuffer();
  for (const char *character = label; *character; ++character) {
    const UiGlyph *glyph = findGlyph(FONT_SMALL, *character);
    if (!glyph) continue;
    for (int row = 0; row < glyph->height; ++row) {
      const int py = y + row;
      if (py < 0 || py >= SCREEN_HEIGHT) continue;
      for (int column = 0; column < glyph->width; ++column) {
        const int px = x + column;
        if (px < 0 || px >= SCREEN_WIDTH) continue;
        const int index = row * glyph->width + column;
        const uint8_t packed = FONT_SMALL.pixels[glyph->offset + index / 2];
        const uint8_t alpha = index % 2 ? packed & 15 : packed >> 4;
        if (alpha) {
          uint16_t &pixel = pixels[py * SCREEN_WIDTH + px];
          pixel = blend565(navInk(pill, px, py), pixel, alpha);
        }
      }
    }
    x += glyph->advance;
  }
}

void navIcon(int x, int y, uint8_t page, const Rect &pill) {
  if (page >= PAGE_COUNT) return;
  static constexpr uint16_t NAV_ICONS[PAGE_COUNT][14] = {
      // PAGE_PET: Companion cat face with ears, eyes, and smile
      {0x2001, 0x3003, 0x3807, 0x3C0F, 0x27F9, 0x2001, 0x2619, 0x2619, 0x2001, 0x2121, 0x10C2, 0x0804, 0x07F8, 0x0000},
      // PAGE_CLOCK: Dial bezel with hour and minute hands
      {0x03F0, 0x0C0C, 0x1002, 0x2001, 0x20C1, 0x20C1, 0x20F9, 0x20C1, 0x2001, 0x1002, 0x0C0C, 0x03F0, 0x0000, 0x0000},
      // PAGE_WEATHER: Cloud with falling rain drops
      {0x01E0, 0x07F8, 0x0804, 0x1002, 0x2001, 0x2001, 0x2001, 0x1FFE, 0x0000, 0x0888, 0x0888, 0x0000, 0x0888, 0x0888},
      // PAGE_SETTINGS: 8-tooth mechanical cog with center axle hole
      {0x01E0, 0x01E0, 0x0CCC, 0x1E1E, 0x0FFC, 0x33F3, 0x3213, 0x3213, 0x33F3, 0x0FFC, 0x1E1E, 0x0CCC, 0x01E0, 0x01E0},
  };

  uint16_t *pixels = canvas.getFramebuffer();
  for (int row = 0; row < 14; ++row) {
    const int py = y + row;
    if (py < 0 || py >= SCREEN_HEIGHT) continue;
    const uint16_t bits = NAV_ICONS[page][row];
    for (int col = 0; col < 14; ++col) {
      if (bits & (1 << (13 - col))) {
        const int px = x + col;
        if (px >= 0 && px < SCREEN_WIDTH) {
          pixels[py * SCREEN_WIDTH + px] = navInk(pill, px, py);
        }
      }
    }
  }
}
}  // namespace

int textWidth(const String &value, const UiFont &font) {
  int width = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    const UiGlyph *glyph = findGlyph(font, value[i]);
    if (glyph) width += glyph->advance;
  }
  return width;
}

void uiText(int x, int y, const String &value, const UiFont &font, uint16_t color, uint16_t background) {
  uint16_t shades[16];
  for (int alpha = 0; alpha < 16; ++alpha) {
    const int red = (((color >> 11) & 31) * alpha + ((background >> 11) & 31) * (15 - alpha)) / 15;
    const int green = (((color >> 5) & 63) * alpha + ((background >> 5) & 63) * (15 - alpha)) / 15;
    const int blue = ((color & 31) * alpha + (background & 31) * (15 - alpha)) / 15;
    shades[alpha] = (red << 11) | (green << 5) | blue;
  }
  uint16_t *pixels = canvas.getFramebuffer();
  for (size_t i = 0; i < value.length(); ++i) {
    const UiGlyph *glyph = findGlyph(font, value[i]);
    if (!glyph) continue;
    for (int row = 0; row < glyph->height; ++row) {
      if (y + row < 0 || y + row >= SCREEN_HEIGHT) continue;
      for (int column = 0; column < glyph->width; ++column) {
        if (x + column < 0 || x + column >= SCREEN_WIDTH) continue;
        const int index = row * glyph->width + column;
        const uint8_t packed = font.pixels[glyph->offset + index / 2];
        const uint8_t alpha = index % 2 ? packed & 15 : packed >> 4;
        if (alpha) pixels[(y + row) * SCREEN_WIDTH + x + column] = shades[alpha];
      }
    }
    x += glyph->advance;
  }
}

void uiCentered(int y, const String &value, const UiFont &font, uint16_t color) {
  uiText((SCREEN_WIDTH - textWidth(value, font)) / 2, y, value, font, color);
}

void uiRightAligned(int y, const String &value, const UiFont &font) {
  uiText(302 - textWidth(value, font), y, value, font, COLOR_TEXT);
}

void header(const char *title) {
  uiText(16, 11, title, FONT_SMALL, COLOR_TEXT);
  const String label = WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE";
  const int width = textWidth(label, FONT_SMALL);
  canvas.fillCircle(294 - width, 17, 2, COLOR_TEXT);
  uiText(302 - width, 11, label, FONT_SMALL, COLOR_TEXT);
}

Rect navPill(uint8_t page) {
  const int width = navContentWidth(page) + 2 * NAV_PILL_PADDING;
  return {page * NAV_TAB_WIDTH + (NAV_TAB_WIDTH - width) / 2, NAV_PILL_Y, width, NAV_PILL_HEIGHT};
}

void footer(const Rect &pill) {
  canvas.drawFastHLine(16, NAV_DIVIDER_Y, 288, COLOR_SURFACE);
  fillPill(pill, COLOR_ACCENT);
  for (uint8_t page = 0; page < PAGE_COUNT; ++page) {
    const int x = page * NAV_TAB_WIDTH + (NAV_TAB_WIDTH - navContentWidth(page)) / 2;
    navIcon(x, NAV_LABEL_Y, page, pill);
    navLabel(x + 20, NAV_LABEL_Y, NAV_LABELS[page], pill);
  }
}

void pill(const Rect &rect, const String &label, uint16_t color) {
  canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, 10, color);
  uiText(rect.x + (rect.w - textWidth(label, FONT_BODY)) / 2, rect.y + 7, label, FONT_BODY, COLOR_TEXT, color);
}

void spinner(int x, int y, uint32_t now) {
  const int start = (now / SPINNER_STEP) % 12 * 30;
  canvas.fillArc(x, y, 7, 4, 0, 360, COLOR_SURFACE);
  const int end = start + 100;
  if (end <= 360) {
    canvas.fillArc(x, y, 7, 4, start, end, COLOR_SKY);
  } else {  // the arc wraps past 0 degrees
    canvas.fillArc(x, y, 7, 4, start, 359.9F, COLOR_SKY);
    canvas.fillArc(x, y, 7, 4, 0, end - 360, COLOR_SKY);
  }
}
}  // namespace ui
