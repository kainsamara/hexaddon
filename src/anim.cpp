#include "anim.h"
#include "sprites.h"
#include "settings.h"
#include "display.h"  
#include <Preferences.h>
#include <cstring>
#include <math.h>

// ============================================================
//  ADDON 2022 - Слайдшоу с оффзон-аддона 22 года
//  OFFZONE 2024 - автономная демка (бык + волки)
//  OFFZONE 2026 - статичный терм + мигающий прямоугольник
// ============================================================

enum class GameState : uint8_t { Intro, Menu, Countdown, Playing, GameOver };
enum class Dir       : uint8_t { Right = 0, UpperRight = 1, Left = 2, UpperLeft = 3 };

static const float BULL_CX     = 64.0f;
static const int   BULL_BOTTOM = 63;

// --- цель (рога быка: верхняя часть спрайта) ---
static const float TARGET_X    = 64.0f;     // 0..128, куда летят волки по X
static const float TARGET_Y    = 44.0f;     // 0..64,  куда летят волки по Y

// --- слева (летит горизонтально в рога) ---
static const float SPAWN_L_X   = -WOLF_W;   // -23 = за левым краем
static const float SPAWN_L_Y   = TARGET_Y;  // высота старта

// --- справа (летит горизонтально в рога) ---
static const float SPAWN_R_X   = 128.0f;     // за правым краем
static const float SPAWN_R_Y   = TARGET_Y;

// --- сверху-слева (по диагонали из угла в рога) ---
static const float SPAWN_UL_X  = -WOLF_W;     // -23 = за левым краем
static const float SPAWN_UL_Y  = -WOLF_W + 4; // -21 = за верхним краем

// --- сверху-справа (по диагонали из угла в рога) ---
static const float SPAWN_UR_X  = 128.0f;      // за правым краем
static const float SPAWN_UR_Y  = -WOLF_W + 4; // за верхним краем
// ============================================================

static uint8_t bullBmp[4][180];
static const uint8_t *wolfPtr[3] = { wolf1_23x23, wolf2_23x23, wolf3_23x23 };

struct Wolf {
  bool    active = false;
  float   x = 0, y = 0;
  uint8_t sprite = 0;
};

struct Ctx {
  bool       active    = false;
  AnimPreset preset    = AnimPreset::Offzone2024;
  GameState  state     = GameState::Intro;
  uint32_t   stateAt   = 0;
  uint32_t   score     = 0;
  uint8_t    lives     = 3;
  Dir        bullDir   = Dir::Right;
  uint32_t   bestScore = 0;
  uint32_t   nextSpawn = 0;
  uint32_t   nextTurn  = 0;
  uint32_t   lastMove  = 0;
  uint32_t   rndSeed   = 0;
  Wolf       wolves[4];
  // ADDON 2022 slideshow
  uint8_t    slidePair   = 0;
  bool       slideGlitch = false;
  uint32_t   slideUntil  = 0;
};

static Ctx ctx[DISP_NUM];
static bool inited = false;

// ---------- ADDON 2022: слайдшоу со глитчем ----------
static const uint32_t A22_DSD = 3500;   // база длительности полного кадра (как DSD в старом коде)

struct SlidePair {
  const uint8_t *glitch;   // вариант с "_" (короткий глитч-кадр)
  const uint8_t *full;     // полный кадр
  uint32_t baseMs;
};

static const SlidePair A22_SLIDES[] = {
  { dconoffzone_,     dconoffzone,     3500 },
  { dc78422_,         dc78422,         A22_DSD },
  { dc7495_,          dc7495,          A22_DSD },
  { dc78412_,         dc78412,         A22_DSD },
  { dc1337_,          dc1337,          A22_DSD },
  { dc7342_,          dc7342,          A22_DSD },
  { b4cksp4ce_,       b4cksp4ce,       A22_DSD },
  { dc78182_WSDC_,    dc78182_WSDC,    A22_DSD },
  { rm_,              rm,              3000 },
};
static const int A22_COUNT = sizeof(A22_SLIDES) / sizeof(A22_SLIDES[0]);

// Состояние слайдшоу хранится в Ctx: slidePair, slideGlitch, slideUntil
static inline uint32_t rnd32(uint32_t &s);
static void tickSlideshow(Ctx &g, uint32_t now) {
  if ((int32_t)(now - g.slideUntil) < 0) return;  // wrap-safe (~49 days)
  if (!g.rndSeed) g.rndSeed = millis() ^ 0xA2202222u;

  if (g.slideGlitch) {
    // глитч-кадр показан → теперь полный кадр той же пары
    g.slideGlitch = false;
    g.slideUntil = now + A22_SLIDES[g.slidePair].baseMs + (rnd32(g.rndSeed) % 2501);
  } else {
    // выбираем случайную пару (не повторяем предыдущую) и показываем глитч
    int p = rnd32(g.rndSeed) % A22_COUNT;
    if (p == g.slidePair) p = (p + 1) % A22_COUNT;
    g.slidePair = p;
    g.slideGlitch = true;
    g.slideUntil = now + 160 + (rnd32(g.rndSeed) % 90);   // random(60,140)
  }
}

// ---------- зеркало ----------
static inline bool xbmGet(const uint8_t *p, int w, int x, int y) {
  int bpl = (w + 7) / 8;
  return p[y * bpl + (x >> 3)] & (0x80 >> (x & 7));
}
static inline void xbmSet(uint8_t *p, int w, int x, int y) {
  int bpl = (w + 7) / 8;
  p[y * bpl + (x >> 3)] |= (0x80 >> (x & 7));
}
static void mirrorH(const uint8_t *src, int w, int h, uint8_t *dst) {
  int bpl = (w + 7) / 8;
  memset(dst, 0, bpl * h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (xbmGet(src, w, x, y))
        xbmSet(dst, w, w - 1 - x, y);
}

// ---------- счёт (NVS) ----------
static uint32_t loadBest() {
  Preferences p; p.begin("hexaddon", true);
  uint32_t b = p.getUInt("best", 0); p.end(); return b;
}
static void saveBest(uint32_t b) {
  Preferences p; p.begin("hexaddon", false);
  // both displays share one key: never lower the stored record
  if (b > p.getUInt("best", 0)) p.putUInt("best", b);
  p.end();
}

// ---------- init ----------
void animBegin() {
  if (inited) return;
  inited = true;

  memcpy(bullBmp[(int)Dir::Right],      bull1_48x30, 180);
  mirrorH(bull1_48x30, BULL1_W, BULL1_H, bullBmp[(int)Dir::Left]);
  memcpy(bullBmp[(int)Dir::UpperRight], bull2_36x36, 180);
  mirrorH(bull2_36x36, BULL2_W, BULL2_H, bullBmp[(int)Dir::UpperLeft]);

  for (int i = 0; i < DISP_NUM; i++)
    ctx[i].bestScore = loadBest();
}

// ---------- утилиты ----------
static inline uint32_t rnd32(uint32_t &s) {
  s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s;
}

static void setState(Ctx &g, GameState s, uint32_t now) {
  g.state = s; g.stateAt = now;
}

static void startGame(Ctx &g, uint32_t now, uint32_t seed) {
  g.score     = 0;
  g.lives     = 3;
  g.bullDir   = Dir::Right;
  g.nextSpawn = now + 500;
  g.nextTurn  = now;
  g.lastMove  = now;
  g.rndSeed   = seed ? seed : 1;
  for (int i = 0; i < 4; i++) g.wolves[i].active = false;
  setState(g, GameState::Countdown, now);
}

static void spawnWolf(Ctx &g, Dir from, uint32_t &r) {
  Wolf &w = g.wolves[(int)from];
  if (w.active) return;
  w.active = true;
  w.sprite = rnd32(r) % 3;

  switch (from) {
    case Dir::Left:       w.x = SPAWN_L_X;  w.y = SPAWN_L_Y;  break;
    case Dir::Right:      w.x = SPAWN_R_X;  w.y = SPAWN_R_Y;  break;
    case Dir::UpperLeft:  w.x = SPAWN_UL_X; w.y = SPAWN_UL_Y; break;
    case Dir::UpperRight: w.x = SPAWN_UR_X; w.y = SPAWN_UR_Y; break;
  }
}

// ---------- логика 2024 ----------
static void bullThink(Ctx &g, uint32_t &r) {
  int bestI = -1; float bestD2 = 1e9f;
  for (int i = 0; i < 4; i++) {
    if (!g.wolves[i].active) continue;
    float dx = g.wolves[i].x - TARGET_X;
    float dy = g.wolves[i].y - TARGET_Y;
    float d2 = dx*dx + dy*dy;
    if (d2 < bestD2) { bestD2 = d2; bestI = i; }
  }
  if (bestI < 0) return;
  if ((rnd32(r) % 100) < 15) g.bullDir = (Dir)(rnd32(r) % 4);
  else                       g.bullDir = (Dir)bestI;
}

static bool comPortVisible(uint32_t score) {
  if (score < 20) return false;
  return (score % 10) < 3;
}

static void tickPlaying(Ctx &g, uint32_t now) {
  uint32_t &r = g.rndSeed;

  if ((int32_t)(now - g.nextTurn) >= 0) {
    bullThink(g, r);
    g.nextTurn = now + 250;
  }

  if ((int32_t)(now - g.lastMove) < 33) return;
  g.lastMove = now;

  float sp = 1.2f + g.score * 0.04f;
  if (sp > 2.8f) sp = 2.8f;

  int activeCount = 0;
  for (int i = 0; i < 4; i++) {
    Wolf &w = g.wolves[i];
    if (!w.active) continue;

    float dx = TARGET_X - w.x;
    float dy = TARGET_Y - w.y;
    float d  = sqrtf(dx*dx + dy*dy);

    bool  fromSide = ((Dir)i == Dir::Left || (Dir)i == Dir::Right);
    float reach    = fromSide ? 34.0f : 28.0f;

    if (d < reach) {
      if (g.bullDir == (Dir)i) {
        g.score++;
        if (g.score > g.bestScore) g.bestScore = g.score;
      } else {
        if (g.lives > 0) g.lives--;
        if (g.lives == 0) {
          saveBest(g.bestScore);  // one NVS write per game, not per point
          setState(g, GameState::GameOver, now);
          w.active = false;
          g.nextSpawn = now + 350;
          return;  // game over: no scoring/respawn in the rest of this tick
        }
      }
      w.active = false;
      g.nextSpawn = now + 350;
      continue;
    }

    w.x += (dx / d) * sp;
    w.y += (dy / d) * sp;
    activeCount++;
  }

  if (activeCount == 0 && (int32_t)(now - g.nextSpawn) >= 0)
    spawnWolf(g, (Dir)(rnd32(r) % 4), r);
}

void animReset(int d) {
  Ctx &g = ctx[d];
  for (int i = 0; i < 4; i++) g.wolves[i].active = false;
  g.score = 0; g.lives = 3; g.bullDir = Dir::Right;
  g.rndSeed = millis() ^ (d * 0x9E3779B9u) ^ 0x1234567u;
  g.stateAt = millis();
  g.state   = GameState::Intro;
}

void animLoop() {
  if (!inited) return;
  uint32_t now = millis();

  for (int d = 0; d < DISP_NUM; d++) {
    Ctx &g = ctx[d];

    if (settings.displayMode[d] != DispMode::Anim) {
      g.active = false;
      continue;
    }

    AnimPreset want = settings.animPreset[d];

    if (!g.active || want != g.preset) {
      g.active = true;
      g.preset = want;
      if (want == AnimPreset::Offzone2024) animReset(d);
      if (want == AnimPreset::Addon2022) {   // первый кадр сразу
        g.slidePair = rnd32(g.rndSeed = (g.rndSeed ? g.rndSeed : (millis() ^ (d * 0x9E3779B9u)))) % A22_COUNT;
        g.slideGlitch = false;
        g.slideUntil = 0;
      }
    }

    if (want == AnimPreset::Offzone2026) continue;   // без стейт-машины

    if (want == AnimPreset::Addon2022) {
      tickSlideshow(g, now);
      continue;
    }

    uint32_t el = now - g.stateAt;
    switch (g.state) {
      case GameState::Intro:
        if (el >= 3000) setState(g, GameState::Menu, now);
        break;
      case GameState::Menu:
        if (el >= 4000) startGame(g, now, millis());
        break;
      case GameState::Countdown:
        if (el >= 3000) {
          setState(g, GameState::Playing, now);
          g.nextSpawn = now + 300;
          g.nextTurn  = now;
          g.lastMove  = now;
        }
        break;
      case GameState::Playing:
        tickPlaying(g, now);
        break;
      case GameState::GameOver:
        if (el >= 5000) setState(g, GameState::Intro, now);
        break;
    }
  }
}

// ---------- рендер 2024 ----------
static void drawScoreTop(Adafruit_GFX &g, uint32_t v) {
  char b[8];
  snprintf(b, sizeof(b), "%u", (unsigned)v);
  g.setTextSize(2);
  g.setTextColor(1);
  int w = (int)strlen(b) * 12;
  g.setCursor((128 - w) / 2, 0);
  g.print(b);
}

static void renderIntro(Adafruit_GFX &g) {
  for (int r = 0; r < 2; r++)
    for (int c = 0; c < 4; c++) {
      const uint8_t *s = wolfPtr[(r * 4 + c) % 3];
      g.drawBitmap(6 + c * 31, 6 + r * 29, s, WOLF_W, WOLF_H, 1);
    }
}

static void renderMenu(Adafruit_GFX &g) {
  g.setTextSize(2);
  const int X = 4, PAD = 2;
  const char *s0 = "New Game";
  int w0 = (int)strlen(s0) * 12;
  g.fillRect(X - PAD, 6, w0 + PAD * 2, 16, 1);
  g.setTextColor(0);
  g.setCursor(X, 7);
  g.print(s0);
  g.setTextColor(1);
  g.setCursor(X, 26);
  g.print("Controls");
  g.setCursor(X, 46);
  g.print("Exit");
}

static void renderCountdown(Adafruit_GFX &g, uint32_t el) {
  int n = 3 - (int)(el / 1000);
  if (n < 1) n = 1;
  char b[2] = { (char)('0' + n), 0 };
  g.setTextSize(3);
  g.setTextColor(1);
  g.setCursor((128 - 18) / 2, 4);
  g.print(b);
}

static void renderPlaying(Adafruit_GFX &g, Ctx &gc) {
  drawScoreTop(g, gc.score);

  bool horiz = (gc.bullDir == Dir::Right || gc.bullDir == Dir::Left);
  int bw = horiz ? BULL1_W : BULL2_W;
  int bh = horiz ? BULL1_H : BULL2_H;
  int bx = (int)BULL_CX - bw / 2;
  int by = BULL_BOTTOM - bh + 1;
  g.drawBitmap(bx, by, bullBmp[(int)gc.bullDir], bw, bh, 1);

  for (int i = 0; i < 4; i++) {
    if (!gc.wolves[i].active) continue;
    const uint8_t *s = wolfPtr[gc.wolves[i].sprite];
    g.drawBitmap((int)gc.wolves[i].x - WOLF_W / 2,
                 (int)gc.wolves[i].y - WOLF_H / 2,
                 s, WOLF_W, WOLF_H, 1);
  }

  {
    char lb[3];
    snprintf(lb, sizeof(lb), "%u", (unsigned)gc.lives);
    int nw     = (int)strlen(lb) * 6;
    int totalW = nw + 2 + HEART_W;
    int startX = 126 - totalW;
    g.setTextSize(1);
    g.setTextColor(1);
    g.setCursor(startX, 57);
    g.print(lb);
    g.drawBitmap(startX + nw + 2, 56, heart_8x8, HEART_W, HEART_H, 1);
  }

  if (comPortVisible(gc.score)) {
    g.setTextSize(1);
    g.setTextColor(1);
    g.setCursor(0, 57);
    g.print("COM-port...");
  }
}

static void renderGameOver(Adafruit_GFX &g, Ctx &gc) {
  drawScoreTop(g, gc.score);

  g.setTextSize(2);
  g.setTextColor(1);
  const char *s = "Game Over";
  int w = (int)strlen(s) * 12;
  g.setCursor((128 - w) / 2, 28);
  g.print(s);

  g.setTextSize(1);
  char b[26];
  snprintf(b, sizeof(b), "Max score: %u", (unsigned)gc.bestScore);
  g.setCursor(0, 57);
  g.print(b);

  char hb[3];
  snprintf(hb, sizeof(hb), "0");
  int nw     = (int)strlen(hb) * 6;
  int totalW = nw + 2 + HEART_W;
  int startX = 126 - totalW;
  g.setCursor(startX, 57);
  g.print(hb);
  g.drawBitmap(startX + nw + 2, 56, heart_8x8, HEART_W, HEART_H, 1);
}

// ---------- рендер 2026 ----------
// Статичный терминал + мигающий прямоугольник 21×48 в (30, 8)
static void renderOffzone2026(Adafruit_GFX &g) {
  g.drawLine(6,20,25,33,1);
  g.drawLine(25,34,6,46,1);
  bool on = (millis() / 1000) & 1;   // 0 = чёрный, 1 = белый
  g.fillRect(32, 9, 21, 48, on ? 1 : 0);
}

void animRender(Adafruit_GFX &gfx, int d) {
  Ctx &g = ctx[d];

  if (g.preset == AnimPreset::Offzone2026) {
    renderOffzone2026(gfx);
    return;
  }

  if (g.preset == AnimPreset::Addon2022) {
    const SlidePair &s = A22_SLIDES[g.slidePair % A22_COUNT];
    gfx.drawBitmap(0, 0, g.slideGlitch ? s.glitch : s.full, 128, 64, 1);
    return;
  }

  uint32_t el = millis() - g.stateAt;
  switch (g.state) {
    case GameState::Intro:     renderIntro(gfx);         break;
    case GameState::Menu:      renderMenu(gfx);          break;
    case GameState::Countdown: renderCountdown(gfx, el); break;
    case GameState::Playing:   renderPlaying(gfx, g);    break;
    case GameState::GameOver:  renderGameOver(gfx, g);   break;
  }
}