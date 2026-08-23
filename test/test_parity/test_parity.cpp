// Dump the exact pixels the built-in apps draw, as a fixture for a port to replay.
//
// `pixelwire` reimplements Time, Temperature and Humidity so the same panel can be driven either by
// this firmware's own apps or over the cable, and "the same" has to mean the same pixels rather
// than the same idea. So this renders them here, with the real font and settled inputs, and prints
// the canvas. The port replays the printed frames and compares.
//
// It is a test rather than a tool because the test environment is the only one that already builds
// `core/` for the host. It asserts nothing beyond the render having happened: the fixture is the
// output, and what checks it lives in the other repository.
//
//   pio test -e native -f test_parity
//
// Lines are `PARITY <case> <width> <height> <rrggbb per pixel, row-major>`.

#include <unity.h>

#include <cstdio>

#include "core/apps/builtin/HumidityApp.h"
#include "core/apps/builtin/TempApp.h"
#include "core/apps/ClockText.h"
#include "core/apps/builtin/TimeApp.h"
#include "media/AwtrixFontAdapter.h"

using namespace awtrix;

void setUp() {}
void tearDown() {}

namespace {

// The font as the firmware holds it, so a port renders from the same glyph table rather than from
// its own reading of the BDF. Two parsers of one font file agree until they do not, and the place
// they disagree is a single column on one letter that nobody notices for a year.
void emitFont() {
  const GfxFont& f = awtrixFont(FontId::Small);
  std::printf("FONT small %u %u %u\n", f.first, f.last, f.yAdvance);
  for (unsigned code = f.first; code <= f.last; ++code) {
    const FontGlyph& g = f.glyphs[code - f.first];
    std::printf("GLYPH %u %u %u %u %d %d ", code, g.width, g.height, g.xAdvance, g.xOffset,
                g.yOffset);
    // The rows, most significant bit first, as the renderer walks them.
    const unsigned bits = static_cast<unsigned>(g.width) * g.height;
    for (unsigned bit = 0; bit < bits; ++bit) {
      const unsigned at = g.bitmapOffset * 8u + bit;
      std::printf("%c", (f.bitmap[at / 8] & (0x80u >> (at % 8))) ? '1' : '0');
    }
    std::printf("\n");
  }
  // The degree sign lives outside the contiguous span, in a sparse range.
  for (unsigned code : {0x00B0u}) {
    for (unsigned r = 0; r < f.rangeCount; ++r) {
      const FontRange& range = f.ranges[r];
      if (code < range.first || code > range.last) continue;
      const uint16_t index = range.index[code - range.first];
      if (!index) continue;
      const FontGlyph& g = f.glyphs[index - 1];
      std::printf("GLYPH %u %u %u %u %d %d ", code, g.width, g.height, g.xAdvance, g.xOffset,
                  g.yOffset);
      const unsigned bits = static_cast<unsigned>(g.width) * g.height;
      for (unsigned bit = 0; bit < bits; ++bit) {
        const unsigned at = g.bitmapOffset * 8u + bit;
        std::printf("%c", (f.bitmap[at / 8] & (0x80u >> (at % 8))) ? '1' : '0');
      }
      std::printf("\n");
    }
  }
}

// The separator's brightness curve, sampled across its whole period. A port of `separatorLevel`
// looks like a transcription and is easy to get subtly wrong - a phase offset, a period, integer
// division on the modulo - and none of that shows up in a still frame.
void emitSeparator() {
  for (int mode : {kSepSteady, kSepBlink, kSepPulse}) {
    std::printf("SEPARATOR %d", mode);
    // Blink is keyed on the second, pulse on the millisecond, so both axes are walked.
    for (int second = 0; second < 4; ++second)
      for (int ms = 0; ms < 2000; ms += 125)
        std::printf(" %.6f", separatorLevel(mode, second, ms));
    std::printf("\n");
  }
}

void emit(const char* name, const Canvas& c) {
  std::printf("PARITY %s %d %d ", name, c.width(), c.height());
  for (int y = 0; y < c.height(); ++y)
    for (int x = 0; x < c.width(); ++x) std::printf("%06X", c.getPixel(x, y) & 0xFFFFFFu);
  std::printf("\n");
}

// One settled clock, so a fixture does not move with the wall clock. Seconds are chosen odd so the
// blinking separator is in its lit half whatever mode the case asks for.
RenderCtx settled(const Settings& s, const RuntimeState& rt) {
  RenderCtx ctx;
  ctx.settings = &s;
  ctx.runtime = &rt;
  ctx.font = &awtrixFont(FontId::Small);
  ctx.fonts[0] = &awtrixFont(FontId::Small);
  ctx.fonts[1] = &awtrixFont(FontId::Large);
  ctx.nowMs = 0;
  ctx.hour = 9;
  ctx.minute = 41;
  ctx.second = 1;
  ctx.weekday = 3;   // Wednesday, counted from Sunday
  ctx.mday = 7;
  ctx.month = 5;
  ctx.year = 2026;
  return ctx;
}

void dumpTime(const char* name, int mode, bool weekdayBar, bool time24h) {
  Settings s;
  s.timeMode = mode;
  s.time24h = time24h;
  s.timeSeparatorMode = kSepSteady;
  s.textColor = 0xFFFFFFu;
  s.timeColor = OptColor{};
  s.weekdayBar.show = weekdayBar;
  RuntimeState rt;
  RenderCtx ctx = settled(s, rt);
  Canvas c(32, 8);
  TimeApp app;
  app.render(c, ctx);
  emit(name, c);
}

}

static void test_dump_parity_frames() {
  emitFont();
  emitSeparator();
  dumpTime("time.plain", 0, false, true);
  dumpTime("time.plain.weekday", 0, true, true);
  dumpTime("time.calendar", 1, false, true);
  dumpTime("time.big", 5, false, true);
  dumpTime("time.binary", 6, false, true);
  dumpTime("time.12h", 0, false, false);

  {
    Settings s;
    s.textColor = 0xFFFFFFu;
    s.temperatureColor = OptColor{};
    s.useCelsius = true;
    RuntimeState rt;
    rt.temperatureC = 21.5f;
    rt.tempDecimals = 0;
    RenderCtx ctx = settled(s, rt);
    Canvas c(32, 8);
    TempApp app;
    app.render(c, ctx);
    emit("temperature.c", c);
  }
  {
    Settings s;
    s.textColor = 0xFFFFFFu;
    s.humidityColor = OptColor{};
    RuntimeState rt;
    rt.humidity = 43.0f;
    RenderCtx ctx = settled(s, rt);
    Canvas c(32, 8);
    HumidityApp app;
    app.render(c, ctx);
    emit("humidity", c);
  }
  TEST_ASSERT_TRUE(true);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_dump_parity_frames);
  return UNITY_END();
}
