#include "leds.h"

#include <Adafruit_NeoPixel.h>
#include <esp_system.h>
#include <math.h>

#include "net.h"
#include "settings.h"

// ---------------- color helpers (minimal FastLED replacement) ----------------
struct Col {
  uint8_t r, g, b;
};

static uint8_t sineTab[256];
static uint8_t gammaTab[256];  // 2.2 gamma: perceptual brightness at the low end

static inline uint8_t sin8(uint8_t x) { return sineTab[x]; }
static inline uint8_t qsub8(uint8_t a, uint8_t b) { return a > b ? (uint8_t)(a - b) : 0; }
static inline uint8_t rnd8() { return (uint8_t)esp_random(); }
static inline uint8_t rnd8(uint8_t n) { return n ? (uint8_t)(esp_random() % n) : 0; }
static inline uint8_t rnd8(uint8_t a, uint8_t b) {
  return (uint8_t)(b > a ? a + (esp_random() % (b - a)) : a);
}

static inline Col scaleCol(Col c, uint8_t s) {
  Col r;
  r.r = (uint8_t)((uint16_t)c.r * s / 255);
  r.g = (uint8_t)((uint16_t)c.g * s / 255);
  r.b = (uint8_t)((uint16_t)c.b * s / 255);
  return r;
}

// brightness applied in linear, then gamma-corrected on the way out:
// a linear scale makes everything below ~30% look like dead grey
static inline Col gammaCol(Col c, uint8_t b) {
  Col r;
  r.r = gammaTab[(uint8_t)((uint16_t)c.r * b / 255)];
  r.g = gammaTab[(uint8_t)((uint16_t)c.g * b / 255)];
  r.b = gammaTab[(uint8_t)((uint16_t)c.b * b / 255)];
  return r;
}

static inline void fadeToBlack(Col &c, uint8_t amt) { c = scaleCol(c, (uint8_t)(255 - amt)); }

static inline void addCol(Col &a, Col b) {
  int t;
  t = a.r + b.r; a.r = t > 255 ? 255 : (uint8_t)t;
  t = a.g + b.g; a.g = t > 255 ? 255 : (uint8_t)t;
  t = a.b + b.b; a.b = t > 255 ? 255 : (uint8_t)t;
}

static inline Col hsv2rgb(uint8_t h, uint8_t s, uint8_t v) {
  Col r;
  if (s == 0) { r.r = r.g = r.b = v; return r; }
  uint8_t region = h / 43;
  uint8_t rem = (uint8_t)((h - region * 43) * 6);
  uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
  uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
  uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
  switch (region) {
    case 0:  r.r = v; r.g = t; r.b = p; break;
    case 1:  r.r = q; r.g = v; r.b = p; break;
    case 2:  r.r = p; r.g = v; r.b = t; break;
    case 3:  r.r = p; r.g = q; r.b = v; break;
    case 4:  r.r = t; r.g = p; r.b = v; break;
    default: r.r = v; r.g = p; r.b = q; break;
  }
  return r;
}

static inline Col heatColor(uint8_t t) {
  Col c;
  if (t < 85) { c.r = (uint8_t)(t * 3); c.g = 0; c.b = 0; }
  else if (t < 170) { t -= 85; c.r = 255; c.g = (uint8_t)(t * 3); c.b = 0; }
  else { t -= 170; c.r = 255; c.g = 255; c.b = (uint8_t)(t * 3); }
  return c;
}

static inline void fillRainbow(Col *out, int n, uint8_t base, uint8_t delta) {
  uint8_t hue = base;
  for (int i = 0; i < n; i++) {
    out[i] = hsv2rgb(hue, 255, 255);
    hue = (uint8_t)(hue + delta);
  }
}

static inline void fillSolid(Col *out, int n, Col c) {
  for (int i = 0; i < n; i++) out[i] = c;
}

// hue 0..255 matching hsv2rgb() scale
static inline uint8_t rgb2hue(Col c) {
  int r = c.r, g = c.g, b = c.b;
  int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  int d = mx - mn;
  if (d == 0) return 0;
  int h;
  if (mx == r) h = 43 * (g - b) / d;
  else if (mx == g) h = 85 + 43 * (b - r) / d;
  else h = 170 + 43 * (r - g) / d;
  while (h < 0) h += 256;
  return (uint8_t)(h & 255);
}

static inline uint8_t hash8(uint8_t x) {
  x ^= x >> 3;
  x = (uint8_t)(x * 0x1D);
  x ^= x >> 5;
  x = (uint8_t)(x * 0x2B);
  x ^= x >> 7;
  return x;
}

// ---------------- hardware ----------------
// NeoPixel writes in wire order itself (NEO_* type flags), no manual swizzle needed.
static Adafruit_NeoPixel strip(MAX_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);

static neoPixelType typeFor(const String &o) {
  uint16_t t = NEO_GRB;
  if (o == "RGB") t = NEO_RGB;
  else if (o == "RBG") t = NEO_RBG;
  else if (o == "GBR") t = NEO_GBR;
  else if (o == "BRG") t = NEO_BRG;
  else if (o == "BGR") t = NEO_BGR;
  return (neoPixelType)(t | NEO_KHZ800);
}

// ---------------- state ----------------
static Col frame[MAX_LEDS];
static Col tmp[MAX_LEDS];
static uint8_t heat[MAX_LEDS];

static uint8_t lastAnim = 0xFF;
static int lastN = -1;
static Scope lastScope = Scope::Panel;
static bool shownBlack = false;

const char *ledsAnimName(uint8_t idx) {
  switch (idx) {
    case 0: return "Радуга";
    case 1: return "Дыхание";
    case 2: return "Комета";
    case 3: return "Искры";
    case 4: return "Плазма";
    case 5: return "Огонь";
    case 6: return "Цвет";
    case 7: return "Волна";
    case 8: return "Бегунок";
    case 9: return "Полиция";
    case 10: return "Театр";
    case 11: return "Мерцание";
    case 12: return "Строб";
    case 13: return "Сдвиг";
    case 14: return "Вспышка";
    case 15: return "Вертушка";
    default: return "?";
  }
}

// for the display (ASCII only)
const char *ledsAnimNameEn(uint8_t idx) {
  switch (idx) {
    case 0: return "Rainbow";
    case 1: return "Breath";
    case 2: return "Comet";
    case 3: return "Sparks";
    case 4: return "Plasma";
    case 5: return "Fire";
    case 6: return "Color";
    case 7: return "Wave";
    case 8: return "Runner";
    case 9: return "Police";
    case 10: return "Theater";
    case 11: return "Twinkle";
    case 12: return "Strobe";
    case 13: return "Scroll";
    case 14: return "Flash";
    case 15: return "Propeller";
    default: return "?";
  }
}

// ---------------- effects ----------------
// out  - buffer to draw into
// n    - number of leds in this segment
// off  - global offset of this segment (for persistent state)
// t    - millis()
// sp   - speed 0..100
// c    - primary color

static void efxRainbow(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint16_t rate = (uint16_t)sp * 5;  // hue units per second, 0 = static
  uint8_t base = (uint8_t)(((uint32_t)((uint64_t)t * rate / 1000)) & 0xFF);
  if (settings.colorRandom) base = (uint8_t)(base + rgb2hue(c));
  uint8_t delta = n > 1 ? (255 + n - 2) / (n - 1) : 0;
  fillRainbow(out, n, base, delta);
}

static void efxBreath(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint16_t period = 6000 - (uint16_t)sp * 55;
  if (period < 400) period = 400;
  uint8_t phase = (uint8_t)((uint32_t)(t % period) * 256 / period);
  uint8_t b = sin8(phase);
  for (int i = 0; i < n; i++) out[i] = scaleCol(c, b);
}

static void efxComet(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint32_t dur = 9000 - (uint32_t)sp * 80;
  if (dur < 600) dur = 600;
  float head = (float)(t % dur) / (float)dur * (n + 8.0f);
  for (int i = 0; i < n; i++) {
    fadeToBlack(out[i], 110);
    float d = head - (float)i;
    if (d >= 0.0f && d < 1.0f) {
      addCol(out[i], c);
    } else if (d >= 1.0f && d < 8.0f) {
      uint8_t b = (uint8_t)(255.0f * (1.0f - (d - 1.0f) / 7.0f));
      addCol(out[i], scaleCol(c, b));
    }
  }
}

static void efxSparkle(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint8_t fade = 40 + sp / 2;
  uint8_t chance = 6 + sp / 5;
  Col white = {255, 255, 255};
  for (int i = 0; i < n; i++) {
    fadeToBlack(out[i], fade);
    if (rnd8() < chance) {
      out[i] = (rnd8() < 96) ? white : c;
    }
  }
}

static void efxPlasma(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint16_t tt = (uint32_t)t * (4 + sp / 8) / 100;
  uint8_t span = n > 1 ? (uint8_t)(255 / n) : 0;
  uint8_t hue = (uint8_t)(t / 40);
  if (settings.colorRandom) hue = (uint8_t)(hue + rgb2hue(c));
  for (int i = 0; i < n; i++) {
    uint8_t x = (uint16_t)i * span;
    uint8_t a = (uint8_t)(x * 2u + tt);
    uint8_t b = (uint8_t)(x * 3u - (uint16_t)tt * 2u);
    uint8_t d = (uint8_t)(tt + i * 8);
    uint8_t v = ((uint16_t)sin8(a) + sin8(b) + sin8(d)) / 3;
    out[i] = hsv2rgb((uint8_t)(hue + v), 230, 255);
  }
}

static void efxFire(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  if (n <= 0) return;
  uint8_t ignite = 12 + sp / 4;
  if (rnd8(100) < ignite) heat[off + rnd8((uint8_t)n)] = 255;
  if (rnd8(100) < 40) heat[off] = 255;

  for (int i = 0; i < n; i++) {
    uint8_t &h = heat[off + i];
    h = qsub8(h, rnd8(16, 52));
  }
  for (int i = 1; i < n - 1; i++) {
    uint16_t s = (uint16_t)heat[off + i - 1] + (uint16_t)heat[off + i] * 2 +
                 heat[off + i + 1];
    heat[off + i] = s >> 2;
  }
  // flame tinted by the selected color: hue of c, whitening toward tips
  int mx = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
  int mn = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
  uint8_t satC = mx ? (uint8_t)((mx - mn) * 255 / mx) : 0;
  uint8_t base = rgb2hue(c);
  for (int i = 0; i < n; i++) {
    uint8_t hv = heat[off + i];
    uint8_t hot = hv > 150 ? (uint8_t)(((uint16_t)(hv - 150) * 255) / 105) : 0;
    uint8_t hue = (uint8_t)(base + (hash8((uint8_t)(off + i)) & 0x1F) - 16);
    uint8_t sat = hot ? (uint8_t)((uint16_t)satC * (255 - hot) / 255) : satC;
    out[i] = hsv2rgb(hue, sat, hv);
  }
}

static void efxSolid(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  fillSolid(out, n, c);
}

static void efxWave(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint16_t freq = (n > 1 ? 510 / n : 255) + 4 + sp / 4;
  uint16_t tt = (uint32_t)t * (20 + (uint16_t)sp * 3) / 1000;
  for (int i = 0; i < n; i++) {
    uint8_t ph = (uint8_t)((int32_t)(i * freq) - (int32_t)tt);
    uint8_t b = sin8(ph);
    out[i] = scaleCol(c, b);
  }
}

// 8 Бегунок — точка с хвостом бежит туда-обратно
static void efxRunner(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  if (n <= 0) return;
  if (n == 1) { out[0] = c; return; }
  uint32_t leg = 500 - (uint32_t)sp * 4;
  if (leg < 40) leg = 40;
  uint32_t steps = (uint32_t)(2 * (n - 1));
  uint32_t total = leg * steps;
  uint32_t s = (t % total) / leg;
  int idx = (int)(s <= (uint32_t)(n - 1) ? s : steps - s);
  fillSolid(out, n, {0, 0, 0});
  out[idx] = c;
  bool up = s <= (uint32_t)(n - 1);
  int behind = up ? idx - 1 : idx + 1;
  if (behind >= 0 && behind < n) out[behind] = scaleCol(c, 110);
}

// 9 Полиция — два сектора мигают попеременно, медленно вращаясь
static void efxPolice(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  if (n <= 0) return;
  uint32_t period = 500 - (uint32_t)sp * 4;
  if (period < 80) period = 80;
  bool a = ((t / period) & 1) == 0;
  Col sec = hsv2rgb((uint8_t)(rgb2hue(c) + 160), 255, 255);
  uint32_t rot = (t / (period * 6)) % (uint32_t)n;
  int half = (n + 1) / 2;
  for (int i = 0; i < n; i++) {
    uint32_t j = ((uint32_t)i + (uint32_t)n - rot) % (uint32_t)n;
    bool first = (int)j < half;
    out[i] = first ? (a ? c : Col{0, 0, 0}) : (a ? Col{0, 0, 0} : sec);
  }
}

// 10 Театр — каждая третья горит, фаза сдвигается
static void efxTheater(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint32_t step = 400 - (uint32_t)sp * 3;
  if (step < 60) step = 60;
  uint32_t ph = (t / step) % 3;
  for (int i = 0; i < n; i++)
    out[i] = (((uint32_t)i + ph) % 3 == 0) ? c : Col{0, 0, 0};
}

// 11 Мерцание — независимые плавные пульсации в палитре выбранного цвета
static void efxTwinkle(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint8_t base = rgb2hue(c);
  uint32_t rate = 8 + sp;
  uint32_t adv = (uint32_t)(t * rate / 1000);
  for (int i = 0; i < n; i++) {
    uint8_t ph = (uint8_t)(adv + hash8((uint8_t)i) * 3u + (uint32_t)i * 23u);
    uint8_t b = sin8(ph);
    uint8_t h = (uint8_t)(base + (hash8((uint8_t)(i + 40)) & 0x3F));
    out[i] = scaleCol(hsv2rgb(h, 255, 255), b);
  }
}

// 12 Строб — короткие вспышки всего ряда, цвет/белый попеременно
static void efxStrobe(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint32_t per = 700 - (uint32_t)sp * 6;
  if (per < 90) per = 90;
  uint32_t idx = t / per;
  uint32_t ph = t % per;
  bool on = ph < per / 8 + 15;
  if (on) {
    Col fl = (idx & 1) ? c : Col{255, 255, 255};
    fillSolid(out, n, fl);
  } else {
    fillSolid(out, n, {0, 0, 0});
  }
}

// 13 Сдвиг — палитра вокруг выбранного цвета едет по ряду
static void efxScroll(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint32_t step = 350 - (uint32_t)sp * 3;
  if (step < 50) step = 50;
  uint32_t shift = t / step;
  uint8_t base = rgb2hue(c);
  for (int i = 0; i < n; i++) {
    uint32_t p = (uint32_t)i + shift;
    out[i] = hsv2rgb((uint8_t)(base + (hash8((uint8_t)(p >> 1)) & 0x7F)), 255, 255);
  }
}

// 14 Вспышка — резкая вспышка всего ряда с затуханием
static void efxPop(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  uint32_t per = 3000 - (uint32_t)sp * 25;
  if (per < 700) per = 700;
  uint32_t ph = t % per;
  uint8_t b;
  if (ph < 60) {
    b = 255;
  } else {
    uint32_t d = ph - 60;
    b = d >= 700 ? 0 : (uint8_t)(255 - (255 * d) / 700);
  }
  fillSolid(out, n, scaleCol(c, b));
}

// 15 Вертушка — три лопасти цветов крутятся по ряду
static void efxPropeller(Col *out, int n, int off, uint32_t t, uint8_t sp, Col c) {
  if (n <= 0) return;
  uint8_t base = rgb2hue(c);
  uint32_t step = 300 - (uint32_t)sp * 2;
  if (step < 40) step = 40;
  uint32_t rot = (t / step) % (uint32_t)n;
  for (int i = 0; i < n; i++) {
    uint32_t j = ((uint32_t)i + (uint32_t)n - rot) % (uint32_t)n;
    uint8_t h = (uint8_t)(base + (uint8_t)(j * 3u / (uint32_t)n) * 85u);
    out[i] = hsv2rgb(h, 255, 255);
  }
}

typedef void (*EfxFn)(Col *, int, int, uint32_t, uint8_t, Col);

struct Efx {
  const char *name;
  EfxFn fn;
};

static const Efx EFXS[] = {
    {"Радуга", efxRainbow},
    {"Дыхание", efxBreath},
    {"Комета", efxComet},
    {"Искры", efxSparkle},
    {"Плазма", efxPlasma},
    {"Огонь", efxFire},
    {"Цвет", efxSolid},
    {"Волна", efxWave},
    {"Бегунок", efxRunner},
    {"Полиция", efxPolice},
    {"Театр", efxTheater},
    {"Мерцание", efxTwinkle},
    {"Строб", efxStrobe},
    {"Сдвиг", efxScroll},
    {"Вспышка", efxPop},
    {"Вертушка", efxPropeller},
};

// ---------------- random color mode (settings.colorRandom) ----------------
// sharp effects re-roll the hue at each effect iteration,
// smooth effects glide from one random hue to the next over an iteration.

static bool animSmooth(int a) {
  switch (a) {
    case 8:   // Бегунок
    case 9:   // Полиция
    case 10:  // Театр
    case 12:  // Строб
    case 14:  // Вспышка
      return false;
    default:
      return true;
  }
}

// rough duration of one effect iteration (mirrors the formulas above)
static uint32_t animPeriod(int a, uint8_t sp, int n) {
  uint32_t p = 3000;
  switch (a) {
    case 0: p = 4000; break;                                       // Радуга
    case 1: p = 6000 - (uint32_t)sp * 55; if (p < 400) p = 400; break;    // Дыхание
    case 2: p = 9000 - (uint32_t)sp * 80; if (p < 600) p = 600; break;    // Комета
    case 3: p = 2500; break;                                       // Искры
    case 4: p = 3500; break;                                       // Плазма
    case 5: p = 5000; break;                                       // Огонь
    case 6: p = 4000; break;                                       // Цвет
    case 7: p = 3000; break;                                       // Волна
    case 8: {                                                      // Бегунок
      uint32_t leg = 500 - (uint32_t)sp * 4;
      if (leg < 40) leg = 40;
      p = leg * (uint32_t)(n > 1 ? 2 * (n - 1) : 2);
      break;
    }
    case 9: {  // Полиция: полный цикл красный/синий
      uint32_t q = 500 - (uint32_t)sp * 4;
      if (q < 80) q = 80;
      p = q * 2;
      break;
    }
    case 10: {  // Театр: полный цикл фазы
      uint32_t q = 400 - (uint32_t)sp * 3;
      if (q < 60) q = 60;
      p = q * 3;
      break;
    }
    case 11: p = 3500; break;                                      // Мерцание
    case 12: p = 700 - (uint32_t)sp * 6; if (p < 90) p = 90; break;       // Строб
    case 13: p = 4000; break;                                      // Сдвиг
    case 14: p = 3000 - (uint32_t)sp * 25; if (p < 700) p = 700; break;   // Вспышка
    case 15: {                                                     // Вертушка
      uint32_t q = 300 - (uint32_t)sp * 2;
      if (q < 40) q = 40;
      p = q * (uint32_t)(n > 0 ? n : 1);
      break;
    }
    default: break;
  }
  if (p > 8000) p = 8000;  // also catches arithmetic underflow
  return p;
}

static Col rndEffColor(int anim, uint8_t sp, int n, uint32_t t) {
  static uint8_t hueFrom = 0, hueTo = 0;
  static uint32_t t0 = 0, iter = 0;
  static bool inited = false;
  static int lastAnim = -1, lastN = -1;
  if (!inited || anim != lastAnim || n != lastN) {
    inited = true;
    lastAnim = anim;
    lastN = n;
    hueFrom = rnd8();
    hueTo = rnd8();
    t0 = t;
    iter = t / animPeriod(anim, sp, n);
  }
  uint32_t per = animPeriod(anim, sp, n);
  if (!animSmooth(anim)) {
    uint32_t it = t / per;
    if (it != iter) {
      iter = it;
      hueFrom = rnd8();
      hueTo = hueFrom;
    }
    return hsv2rgb(hueFrom, 255, 255);
  }
  if (t - t0 >= per) {
    hueFrom = hueTo;
    hueTo = rnd8();
    t0 = t;
  }
  uint8_t prog = (uint8_t)(((uint64_t)(t - t0) * 256) / per);
  uint8_t diff = (uint8_t)(hueTo - hueFrom);
  uint8_t hue = (uint8_t)(hueFrom + (uint16_t)diff * prog / 256);
  return hsv2rgb(hue, 255, 255);
}

// ---------------- public API ----------------

void ledsInit() {
  for (int i = 0; i < 256; i++)
    sineTab[i] = (uint8_t)(127.5 + 127.5 * sin(i * (TWO_PI / 256.0)));
  for (int i = 0; i < 256; i++)
    gammaTab[i] = (uint8_t)(powf(i / 255.0f, 2.2f) * 255.0f + 0.5f);
  strip.begin();
  ledsApply();
  ledsClearFrame();
  // boot sanity flash: green = firmware drives the strip, then off
  for (int i = 0; i < MAX_LEDS; i++) strip.setPixelColor(i, 0, 80, 0);
  strip.show();
  delay(400);
  strip.clear();
  strip.show();
}

void ledsApply() {
  // brightness + power limit are applied per frame in ledsLoop();
  // here we push the wire order and the pixel count into the driver.
  strip.updateType(typeFor(settings.order));
  int n = settings.ledCount();
  if (n > 0 && (int)strip.numPixels() != n) {
    // blank the tail before shrinking the buffer: leftover pixels would
    // freeze on their last color once show() stops sending their data
    for (uint16_t i = 0; i < strip.numPixels(); i++) strip.setPixelColor(i, 0, 0, 0);
    strip.show();
    strip.updateLength((uint16_t)n);
  }
}

void ledsClearFrame() {
  ::memset(frame, 0, sizeof(frame));
  ::memset(heat, 0, sizeof(heat));
  lastAnim = 0xFF;
  lastScope = settings.scope;  // already zeroed here, no re-clear needed
}

static void portalFx(int n, uint32_t t) {
  for (int i = 0; i < n; i++) {
    uint8_t b = sin8((uint8_t)(t / 8 + i * 30));
    frame[i] = hsv2rgb(160, 255, b);
  }
}

// last measured strip current (after the power cap) + its short-window
// average: strobe/flash effects jump 0..N frame to frame, so /api/status
// reports the average (Q8 fixed point, tau = 3 s). Snapped to 0 at once
// when the strip is off (rail down / backlight out) — no exponential tail.
static uint32_t lastMa = 0;
static uint32_t avgMaQ8 = 0;
static uint32_t avgT = 0;

#define MA_TAU_MS 3000UL

uint32_t ledsCurrentMa() { return avgMaQ8 >> 8; }

static void avgMaStep(uint32_t ma, uint32_t now) {
  uint32_t dt = now - avgT;
  avgT = now;
  if (dt > MA_TAU_MS) dt = MA_TAU_MS;  // after a stall: jump to the value
  uint32_t a = dt * 256 / MA_TAU_MS;   // 0..256 = weight of the new sample
  if (a > 256) a = 256;
  avgMaQ8 += (uint32_t)((int32_t)((ma << 8) - avgMaQ8) * (int32_t)a / 256);
}

void ledsLoop() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 40) return;  // 25 fps
  last = now;

  int n = settings.ledCount();
  if (n > MAX_LEDS) n = MAX_LEDS;
  if (n != lastN) {
    ledsClearFrame();
    lastN = n;
  }

  bool portal = netIsPortal();

  if (portal) {
    portalFx(n, now);
    shownBlack = false;
  } else if (!settings.animOn) {
    bool any = false;
    for (int i = 0; i < n; i++) {
      if (frame[i].r | frame[i].g | frame[i].b) {
        fadeToBlack(frame[i], 48);
        any = true;
      }
    }
    if (!any) {
      lastMa = 0;   // dark strip: /api/status must not show a stale draw
      avgMaQ8 = 0;  // off is off — snap the average too, no decay tail
      if (shownBlack) return;
      shownBlack = true;
    } else {
      shownBlack = false;
    }
  } else {
    shownBlack = false;
    // a scope switch re-segments the strip (off changes): stateful effects
    // (Comet/Sparkle/Fire) must not keep their tails at the old positions
    if (settings.anim != lastAnim || settings.scope != lastScope) {
      ::memset(frame, 0, sizeof(frame));
      ::memset(heat, 0, sizeof(heat));
      lastAnim = settings.anim;
      lastScope = settings.scope;
    }
    const Efx &e = EFXS[settings.anim % NUM_ANIMS];
    Col c = {settings.colR, settings.colG, settings.colB};
    if (settings.colorRandom)
      // Panel scope iterates per 5-LED segment: its period is based on
      // PANEL_LEDS, not the whole strip, or the hue re-rolls too rarely
      c = rndEffColor(settings.anim % NUM_ANIMS, settings.speed,
                      settings.scope == Scope::Panel ? PANEL_LEDS : n, now);
    if (settings.scope == Scope::Chain) {
      e.fn(frame, n, 0, now, settings.speed, c);
    } else {
      for (int p = 0; p < settings.panels && p * PANEL_LEDS + PANEL_LEDS <= MAX_LEDS; p++) {
        e.fn(frame + p * PANEL_LEDS, PANEL_LEDS, p * PANEL_LEDS, now, settings.speed, c);
      }
    }
  }

  // compose: reverse + brightness + power limit, push to hardware buffer
  uint8_t bri = settings.brightness;
  bool rev = settings.reverse;
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) {
    tmp[i] = gammaCol(frame[rev ? (n - 1 - i) : i], bri);
    sum += (uint32_t)tmp[i].r + tmp[i].g + tmp[i].b;
  }
  // ~20 mA per channel at value 255; cap total draw at powerMa
  uint32_t limit = settings.powerMa ? settings.powerMa : 1000000;
  uint32_t mA = (sum * 20) / 255;
  if (mA > limit) {
    uint8_t mul = (uint8_t)((uint64_t)limit * 255 / mA);
    for (int i = 0; i < n; i++) tmp[i] = scaleCol(tmp[i], mul);
    mA = limit;  // what we actually draw after the cap
  }
  // The strip hangs on the addon power rail: with the rail off it has no
  // 5V feed and physically draws nothing, however bright the computed
  // frame is. The frame keeps rendering (so the panel lights up the
  // instant the rail returns) — but the phantom draw must not be reported.
  if (!settings.addonPower) {
    lastMa = 0;
    avgMaQ8 = 0;  // nothing flows at all: report 0 right away
  } else {
    lastMa = mA;
    avgMaStep(mA, now);  // the UI shows the smoothed value
  }
  for (int i = 0; i < MAX_LEDS; i++) {
    if (i < n) strip.setPixelColor(i, tmp[i].r, tmp[i].g, tmp[i].b);
    else strip.setPixelColor(i, 0, 0, 0);
  }
  strip.show();
  static bool firstFrame = true;
  if (firstFrame) {
    firstFrame = false;
    Serial.println("[leds] first frame shown");
  }
}
