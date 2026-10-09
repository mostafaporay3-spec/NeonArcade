#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "driver/i2s.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#ifndef ST77XX_GRAY
#define ST77XX_GRAY 0x7BEF
#endif

struct Step { uint16_t f0, f1, ms; uint8_t vol, mod; }; 
struct RcCar { float z, lx, sp; uint16_t col; bool on, pass; };
struct RcCoin { float z, lx; bool on; };
struct CarSpec { const char *nm; uint16_t body, dark, accent, price; float spd, acc, hnd, nit; uint8_t kind; };
typedef void (*JobFn)(int, int);
struct Wk { float x, y; int cx, cy, tx, ty; };
struct NmSave { uint32_t magic; int lvl, score; };
struct AnSave { uint32_t magic; int lvl, score; };
struct FoSave { uint32_t magic, seed; float x, y, a, bat; int score; uint16_t mask; };
typedef uint16_t (*WallFn)(int, int, int, float, float, int);
typedef uint8_t (*ShapeFn)(float, float);
typedef uint16_t (*ColFn)(uint8_t, int);
struct FpsV { const uint8_t *m; int w, h; float x, y, a; int hz, amb, famb; float fk, wh; WallFn wf; const uint16_t *cl, *fl; uint16_t fog; int fogK; bool ccone; };


#define OLED_CLK 14
#define OLED_MOSI 13
#define OLED_RESET 12
#define OLED_DC 11
#define OLED_CS 10
SPIClass spiOLED(FSPI);
Adafruit_SSD1306 oled(128, 64, &spiOLED, OLED_DC, OLED_RESET, OLED_CS);


#define OLED_BLUE_Y 16

#define TFT_CS 16
#define TFT_RST 15
#define TFT_DC 7
#define TFT_MOSI 6
#define TFT_SCLK 5
#define TFT_BL 4
SPIClass spiTFT(HSPI);

class TFTX : public Adafruit_ST7735 {
 public:
  TFTX(SPIClass *s, int8_t cs, int8_t dc, int8_t rst) : Adafruit_ST7735(s, cs, dc, rst) {}
  void setOffset(int8_t col, int8_t row) { setColRowStart(col, row); }
};
TFTX tft(&spiTFT, TFT_CS, TFT_DC, TFT_RST);

#define BUZZER_PIN 1
#define BATTERY_PIN 18


#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define BL_INIT()   ledcAttach(TFT_BL, 5000, 8)
  #define BL_WRITE(v) ledcWrite(TFT_BL, v)
  #define BL_DETACH() ledcDetach(TFT_BL)
#else
  #define BL_INIT()   do { ledcSetup(4, 5000, 8); ledcAttachPin(TFT_BL, 4); } while (0)
  #define BL_WRITE(v) ledcWrite(4, v)
  #define BL_DETACH() ledcDetachPin(TFT_BL)
#endif


#define SW 128
#define SH 128
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))
#define C_K 0x0000
#define C_W 0xFFFF
class Canv : public Adafruit_GFX {   
 public:
  uint16_t *buf;
  Canv(int16_t w, int16_t h) : Adafruit_GFX(w, h) {
    buf = (uint16_t *)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint16_t *)malloc((size_t)w * h * 2);
    memset(buf, 0, (size_t)w * h * 2);
  }
  uint16_t *getBuffer() { return buf; }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override { if ((unsigned)x >= (unsigned)WIDTH || (unsigned)y >= (unsigned)HEIGHT) return; buf[y * WIDTH + x] = c; }
  void fillScreen(uint16_t c) override { for (int i = 0; i < WIDTH * HEIGHT; i++) buf[i] = c; }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) override {
    if (y < 0 || y >= HEIGHT || w <= 0) return; int x1 = x + w; if (x < 0) x = 0; if (x1 > WIDTH) x1 = WIDTH;
    uint16_t *p = buf + y * WIDTH; for (int i = x; i < x1; i++) p[i] = c;
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) override {
    if (x < 0 || x >= WIDTH || h <= 0) return; int y1 = y + h; if (y < 0) y = 0; if (y1 > HEIGHT) y1 = HEIGHT;
    for (int j = y; j < y1; j++) buf[j * WIDTH + x] = c;
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) override {
    int x1 = x + w, y1 = y + h; if (x < 0) x = 0; if (y < 0) y = 0; if (x1 > WIDTH) x1 = WIDTH; if (y1 > HEIGHT) y1 = HEIGHT;
    for (int j = y; j < y1; j++) { uint16_t *p = buf + j * WIDTH; for (int i = x; i < x1; i++) p[i] = c; }
  }
};
Canv cv(SW, SH);
uint16_t *FB;
static uint16_t FB2[SW * SH];

static inline void hl(int x0, int x1, int y, uint16_t c) {   // خط أفقي سريع بقص تلقائي
  if (y < 0 || y >= SH) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= SW) x1 = SW - 1;
  if (x1 < x0) return;
  uint16_t *p = FB + y * SW;
  for (int x = x0; x <= x1; x++) p[x] = c;
}
static inline void px(int x, int y, uint16_t c) { if ((unsigned)x < SW && (unsigned)y < SH) FB[y * SW + x] = c; }
static inline uint8_t hh(uint32_t i) { return (i * 2654435761u) >> 24; }

enum { K_UP, K_DN, K_LF, K_RT, K_A, K_B, K_ST, K_SE };
const uint8_t PIN_BTN[8] = {19, 20, 21, 47, 48, 38, 39, 40};
const char *BTN_NAME[8] = {"B19", "B20", "B21", "B47", "B48", "B38", "B39", "B40"};
bool rawH[8], rawP[8], held[8], prs[8], rpt[8];
float holdT[8];
uint32_t lastAct = 0;


struct Cfg {
  uint32_t magic;
  uint8_t bl, oledC, sound, vol, music, hud, sleepIdx, wakeIdx, cpuIdx, spiIdx;
  int8_t colS, rowS;
  uint8_t map[8];
  float vfac, toff;
} cfg;
#define CFG_MAGIC 0x4E410003
const uint8_t SLEEP_MIN[5] = {0, 1, 2, 5, 10};
const uint16_t CPU_MHZ[3] = {80, 160, 240};
const uint8_t SPI_MHZ[3] = {20, 27, 40};
Preferences pref;
bool cfgDirty = false; uint32_t cfgT = 0;

void defCfg() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC; cfg.bl = 80; cfg.oledC = 207; cfg.sound = 1; cfg.vol = 3; cfg.music = 1; cfg.hud = 1;
  cfg.sleepIdx = 3; cfg.wakeIdx = 2; cfg.cpuIdx = 2; cfg.spiIdx = 2;
  cfg.colS = 2; cfg.rowS = 3; cfg.vfac = 2.0f; cfg.toff = 12.0f;
  for (int i = 0; i < 8; i++) cfg.map[i] = i;
}
bool loadCfg() {
  Cfg t; size_t n = pref.getBytes("cfg", &t, sizeof(t));
  if (n == sizeof(Cfg) && t.magic == CFG_MAGIC) { cfg = t; return true; }
  defCfg(); return false;
}
void saveCfgNow() { pref.putBytes("cfg", &cfg, sizeof(cfg)); cfgDirty = false; }
void touchCfg() { cfgDirty = true; cfgT = millis(); }

// ---------- الحالة العامة ----------
enum { GM_SNAKE, GM_BREAK, GM_FLAPPY, GM_DASH, GM_STAR, GM_TETRIS, GM_PONG, GM_RACER, GM_HORROR, GM_AST, GM_CRAFT, GM_HIDE, GM_ANOM, GM_FOREST, GM_COUNT };
int score, lives, cur = 0, sel = 0;
bool over, newBest, paused, won, noPause = false;
float shake, gdt;
enum { S_MENU, S_PLAY, S_SET } state = S_MENU;
int hiS[16];
volatile float batV = 4.0f, batPinV = 0, tempRaw = 30, tempC = 30, tempShow = 30, fps = 0;
volatile int batPct = 0;
volatile bool hot = false, lowBat = false, noBat = false, thermalDim = false, reqSleep = false, charging = false;
volatile bool sleeping = false, oledCtrDirty = true;
bool idleDim = false, batInit = false, tempInit = false;
float batSm = 0;
uint8_t sens = 3; static inline float sensM() { return 0.5f + 0.35f * sens; }
const char *oledHelp = ""; char oledDyn[48] = ""; char oledX[24] = "";
SemaphoreHandle_t semGo, semDone, semJ, semJD; JobFn jobFn; volatile int jobA, jobB;
volatile int pSx = 0, pSy = 0;

void die();
void goSleep();
void drawHud();
void onLeave();
void storyStart(int id);
bool storyUpd(float dt);
void storyDraw();
void svMenuDraw(const char *title, const char *info);
void lutInit();


#define SQN 48
Step sq[SQN]; volatile uint8_t sqH = 0, sqT = 0;
Step curStep; uint32_t stepT0 = 0;
volatile bool stepActive = false, cancelCur = false, musicOn = false, audioMute = false;
portMUX_TYPE amux = portMUX_INITIALIZER_UNLOCKED;
#define EIGHTH 250
// لحن أصلي هادي (لا ماينور) - ثُمن = 250ms
const uint8_t MEL[][2] = {
  {69,1},{72,1},{76,1},{72,1},{69,1},{72,1},{76,2},
  {65,1},{69,1},{72,1},{69,1},{65,1},{69,1},{72,2},
  {67,1},{72,1},{76,1},{72,1},{67,1},{72,1},{76,1},{79,1},
  {74,1},{71,1},{67,1},{71,1},{74,1},{71,1},{67,2},
  {76,3},{81,3},{79,2},
  {77,2},{76,2},{72,2},{69,2},
  {74,2},{77,2},{76,2},{74,2},
  {72,6},{0,2}
};
const int MELN = sizeof(MEL) / sizeof(MEL[0]);

const Step SFX_MOVE[]  = {{988, 1100, 28, 120, 0}};
const Step SFX_OK[]    = {{784, 0, 55, 150, 0}, {1047, 0, 120, 160, 0}};
const Step SFX_BACK[]  = {{700, 520, 100, 140, 0}};
const Step SFX_JUMP[]  = {{330, 640, 100, 165, 0}};
const Step SFX_EAT[]   = {{988, 0, 50, 165, 0}, {1319, 0, 110, 160, 0}};
const Step SFX_HIT[]   = {{240, 150, 100, 200, 2}};
const Step SFX_BOOM[]  = {{200, 90, 300, 230, 2}};
const Step SFX_LASER[] = {{1400, 800, 50, 100, 0}};
const Step SFX_DIE[]   = {{523, 0, 100, 175, 0}, {440, 0, 100, 175, 0}, {349, 0, 120, 175, 0}, {262, 196, 420, 190, 1}};
const Step SFX_WIN[]   = {{523, 0, 100, 170, 0}, {659, 0, 100, 170, 0}, {784, 0, 100, 170, 0}, {1047, 0, 380, 190, 1}};
const Step SFX_POWER[] = {{659, 0, 60, 160, 0}, {784, 0, 60, 160, 0}, {988, 0, 60, 160, 0}, {1319, 0, 170, 170, 1}};
const Step SFX_HEART[] = {{120, 90, 70, 235, 0}, {0, 0, 80, 0, 0}, {110, 90, 95, 215, 0}};
const Step SFX_SCARE[] = {{900, 300, 120, 255, 2}, {1800, 200, 380, 255, 2}, {300, 110, 380, 255, 2}};
const Step SFX_GROWL[] = {{160, 110, 500, 200, 2}};
const Step SFX_CREAK[] = {{140, 210, 320, 150, 3}, {210, 120, 420, 150, 3}};
const Step SFX_BOOT[]  = {{523, 0, 120, 150, 0}, {659, 0, 120, 150, 0}, {784, 0, 120, 150, 0}, {1047, 0, 320, 170, 1}};
const Step SFX_SLEEP[] = {{1047, 0, 90, 150, 0}, {784, 0, 100, 150, 0}, {523, 392, 340, 150, 1}};
const Step SFX_TICK[]  = {{1500, 0, 16, 100, 0}};

uint8_t spkMode = 1; bool spkOk = false, spkActive = false; volatile bool audioHalt = false;
ledc_channel_config_t g_cc; uint32_t aoLastD = 0xFFFF;
#define SPK_SR 16000
#define SPK_N 64
static int16_t sinT[256]; static int16_t echoB[2400]; static int echoI = 0;
static inline float wv(uint32_t ph) { return sinT[ph >> 24] * (1.0f / 32768.0f); }
static inline float wvh(uint32_t ph) { float x = wv(ph) * 2.2f; x = x > 1 ? 1 : (x < -1 ? -1 : x); return 0.75f * x + 0.25f * wv(ph * 2); }
void i2sPins() {
  i2s_pin_config_t p = {}; p.mck_io_num = I2S_PIN_NO_CHANGE; p.bck_io_num = I2S_PIN_NO_CHANGE; p.ws_io_num = I2S_PIN_NO_CHANGE;
  p.data_out_num = BUZZER_PIN; p.data_in_num = I2S_PIN_NO_CHANGE; i2s_set_pin(I2S_NUM_0, &p);
}
void audioInit() {
  ledc_timer_config_t tc = {};
  tc.speed_mode = LEDC_LOW_SPEED_MODE; tc.duty_resolution = LEDC_TIMER_10_BIT;
  tc.timer_num = LEDC_TIMER_3; tc.freq_hz = 2000; tc.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&tc);
  g_cc = {};
  g_cc.gpio_num = BUZZER_PIN; g_cc.speed_mode = LEDC_LOW_SPEED_MODE; g_cc.channel = LEDC_CHANNEL_7;
  g_cc.intr_type = LEDC_INTR_DISABLE; g_cc.timer_sel = LEDC_TIMER_3; g_cc.duty = 0; g_cc.hpoint = 0;
  ledc_channel_config(&g_cc);
  for (int i = 0; i < 256; i++) sinT[i] = (int16_t)(sinf(i * 6.2831853f / 256.0f) * 32767);
  i2s_config_t c = {};
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
  c.sample_rate = SPK_SR; c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; c.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_I2S; c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count = 6; c.dma_buf_len = 128; c.use_apll = false; c.tx_desc_auto_clear = true;
  spkOk = (i2s_driver_install(I2S_NUM_0, &c, 0, NULL) == ESP_OK);
}
void audioOut(float f, float v) {
  static float lastF = 0;
  uint32_t d = 0;
  if (f >= 90 && v > 0.003f && cfg.sound && !audioMute) {
    float mx = 1023.0f * 0.06f * cfg.vol;
    d = (uint32_t)(v * mx); if (d > 512) d = 512;
  }
  if (d && fabsf(f - lastF) >= 1.0f) { ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3, (uint32_t)f); lastF = f; }
  if (d != aoLastD) { ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, d); ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7); aoLastD = d; }
}

void spkBlock() {
  static int16_t buf[SPK_N]; static uint32_t phA = 0, phB = 0, rng = 2463534242u; static float nz = 0, stEl = 0, mT = 0, mf = 0; static int mi = 0; static bool mNew = true;
  portENTER_CRITICAL(&amux);
  if (cancelCur) { cancelCur = false; stepActive = false; }
  if (!stepActive && sqT != sqH) { curStep = sq[sqT]; sqT = (sqT + 1) % SQN; stepActive = true; stEl = 0; }
  portEXIT_CRITICAL(&amux);
  bool on = cfg.sound && !audioMute; float gain = (0.45f + cfg.vol * 0.22f) * 1.5f; static float px0 = 0; const float dtm = 1000.0f / SPK_SR;
  for (int i = 0; i < SPK_N; i++) {
    float s = 0;
    if (on) {
      if (stepActive) {
        if (stEl >= curStep.ms) stepActive = false;
        else {
          float t = stEl / curStep.ms, f = curStep.f0; if (curStep.f1) f += ((float)curStep.f1 - curStep.f0) * t;
          float att = fminf(8.0f, curStep.ms * 0.25f), rel = fminf(40.0f, curStep.ms * 0.4f);
          float a = stEl < att ? stEl / att : 1.0f, r = (curStep.ms - stEl) < rel ? (curStep.ms - stEl) / rel : 1.0f;
          float v = curStep.vol / 255.0f * a * r * (1.0f - 0.45f * t);
          if (curStep.mod == 1) f *= 1.0f + 0.012f * sinf(stEl * 0.035f); else if (curStep.mod == 3) v *= 0.6f + 0.4f * sinf(stEl * 0.05f);
          if (f > 20 && f < 7000) {
            phA += (uint32_t)(f * 268435.456f);
            if (curStep.mod == 2) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; float n = (int32_t)rng * (1.0f / 2147483648.0f); nz += (n - nz) * fminf(0.9f, f / 2200.0f + 0.08f); s = (nz * 1.6f + wv(phA) * 0.35f) * v; }
            else s = wvh(phA) * v;
          }
          stEl += dtm;
        }
      } else if (musicOn && cfg.music) {
        mT += dtm; float dur = MEL[mi][1] * EIGHTH;
        if (mT >= dur) { mT -= dur; mi = (mi + 1) % MELN; dur = MEL[mi][1] * EIGHTH; mNew = true; }
        if (mNew) { mNew = false; mf = MEL[mi][0] ? 440.0f * powf(2.0f, (MEL[mi][0] - 69) / 12.0f) : 0; }
        if (mf > 0 && mT < dur * 0.94f) {
          float a = mT < 25 ? mT / 25.0f : 1.0f, r = (dur * 0.94f - mT) < 80 ? (dur * 0.94f - mT) / 80.0f : 1.0f, d = 1.0f - 0.5f * mT / dur;
          phA += (uint32_t)(mf * 268435.456f); phB += (uint32_t)(mf * 0.5f * 268435.456f);
          s = (wv(phA) * 0.7f + wv(phA * 2) * 0.18f * d + wv(phA * 3) * 0.06f * d) * 0.95f * a * r * d + wv(phB) * 0.35f * a * r;
        }
      }
    }
    s *= gain; { float x0 = s; s = x0 + 0.8f * (x0 - px0); px0 = x0; }
    s += echoB[echoI] * (1.0f / 32768.0f) * 0.3f;
    s = tanhf(s * 1.6f);
    echoB[echoI] = (int16_t)(s * 14000); echoI = (echoI + 1) % 2400;
    buf[i] = (int16_t)(s * 30000);
  }
  size_t w = 0; i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, portMAX_DELAY);
}
void audioTask(void *) {
  uint32_t last = millis(); float mT = 0; int mi = 0; float mf = 0; bool mNew = true;
  for (;;) {
    if (audioHalt) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    bool want = spkMode && spkOk;
    if (want != spkActive) {
      spkActive = want; portENTER_CRITICAL(&amux); stepActive = false; sqT = sqH; portEXIT_CRITICAL(&amux);
      if (want) i2sPins(); else { i2s_zero_dma_buffer(I2S_NUM_0); ledc_channel_config(&g_cc); aoLastD = 0xFFFF; }
      last = millis();
    }
    if (spkActive) { spkBlock(); continue; }
    uint32_t now = millis(); float dt = (float)(now - last); last = now;
    portENTER_CRITICAL(&amux);
    if (cancelCur) { cancelCur = false; stepActive = false; }
    if (!stepActive && sqT != sqH) { curStep = sq[sqT]; sqT = (sqT + 1) % SQN; stepActive = true; stepT0 = now; }
    portEXIT_CRITICAL(&amux);
    float f = 0, v = 0;
    if (stepActive) {
      uint32_t el = now - stepT0;
      if (el >= curStep.ms) stepActive = false;
      else {
        float t = (float)el / curStep.ms;
        f = curStep.f0; if (curStep.f1) f += ((float)curStep.f1 - curStep.f0) * t;
        float att = fminf(8.0f, curStep.ms * 0.25f), rel = fminf(40.0f, curStep.ms * 0.4f);
        float a = el < att ? el / att : 1.0f;
        float r = (curStep.ms - el) < rel ? (curStep.ms - el) / rel : 1.0f;
        v = curStep.vol / 255.0f * a * r * (1.0f - 0.45f * t);
        if (curStep.mod == 1) f *= 1.0f + 0.012f * sinf(el * 0.035f);
        else if (curStep.mod == 2) { f *= 1.0f + random(-60, 61) / 1000.0f; v *= 0.7f + random(0, 31) / 100.0f; }
        else if (curStep.mod == 3) v *= 0.6f + 0.4f * sinf(el * 0.05f);
      }
    } else if (musicOn && cfg.music && cfg.sound && !audioMute) {
      mT += dt;
      float dur = MEL[mi][1] * EIGHTH;
      if (mT >= dur) { mT -= dur; mi = (mi + 1) % MELN; dur = MEL[mi][1] * EIGHTH; mNew = true; }
      if (mNew) { mNew = false; mf = MEL[mi][0] ? 440.0f * powf(2.0f, (MEL[mi][0] - 69) / 12.0f) : 0; }
      if (mf > 0 && mT < dur * 0.94f) {
        float a = mT < 25 ? mT / 25.0f : 1.0f;
        float r = (dur * 0.94f - mT) < 60 ? (dur * 0.94f - mT) / 60.0f : 1.0f;
        f = mf; v = 0.55f * a * r * (1.0f - 0.4f * mT / dur);
        if (MEL[mi][1] >= 3 && mT > 150) f *= 1.0f + 0.005f * sinf(mT * 0.034f);
      }
    }
    audioOut(f, v);
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}
void sfxPush(const Step *s, uint8_t n, bool hi) {
  if (!cfg.sound || audioMute) return;
  portENTER_CRITICAL(&amux);
  if (hi) { sqT = sqH; cancelCur = true; }
  else if (sqT != sqH || stepActive) { portEXIT_CRITICAL(&amux); return; }
  for (uint8_t i = 0; i < n; i++) { uint8_t nh = (sqH + 1) % SQN; if (nh == sqT) break; sq[sqH] = s[i]; sqH = nh; }
  portEXIT_CRITICAL(&amux);
}
#define SFX(arr) sfxPush(arr, sizeof(arr) / sizeof(arr[0]), true)
#define SFXL(arr) sfxPush(arr, sizeof(arr) / sizeof(arr[0]), false)
void audioFlush() { portENTER_CRITICAL(&amux); sqT = sqH; cancelCur = true; portEXIT_CRITICAL(&amux); }
bool audioIdle() { return sqT == sqH && !stepActive; }
void snd(int f, int ms) { if (ms < 24) ms = 24; Step s = {(uint16_t)f, 0, (uint16_t)ms, 150, 0}; sfxPush(&s, 1, true); }
void sndL(int f, int ms) { if (ms < 24) ms = 24; Step s = {(uint16_t)f, 0, (uint16_t)ms, 130, 0}; sfxPush(&s, 1, false); }

// ---------- أدوات الرسم ----------
uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return (((ar * (255 - t) + br * t) / 255) << 11) | (((ag * (255 - t) + bg * t) / 255) << 5) | ((ab * (255 - t) + bb * t) / 255);
}
uint16_t wheel(uint8_t p) {
  if (p < 85) return RGB(255 - p * 3, p * 3, 0);
  if (p < 170) { p -= 85; return RGB(0, 255 - p * 3, p * 3); }
  p -= 170; return RGB(p * 3, 0, 255 - p * 3);
}
void vgrad(int y0, int y1, uint16_t c0, uint16_t c1) {
  int n = y1 - y0; if (n < 1) n = 1;
  for (int y = y0; y < y1; y++) hl(0, SW - 1, y, mix(c0, c1, (y - y0) * 255 / n));
}
void txt(const char *s, int x, int y, uint16_t c, uint8_t sz = 1) {
  int w = strlen(s) * 6 * sz; if (x + w > SW) x = SW - w; if (x < 0) x = 0;
  cv.setTextSize(sz); cv.setTextColor(c); cv.setCursor(x, y); cv.print(s);
}
void txtC(const char *s, int y, uint16_t c, uint8_t sz = 1) { txt(s, (SW - (int)strlen(s) * 6 * sz) / 2, y, c, sz); }
void txtS(const char *s, int x, int y, uint16_t c, uint8_t sz = 1) { txt(s, x + 1, y + 1, C_K, sz); txt(s, x, y, c, sz); }
void txtRainbow(const char *s, int y, uint8_t sz, uint8_t off) {
  int n = strlen(s), x = (SW - n * 6 * sz) / 2;
  for (int i = 0; i < n; i++) { char b[2] = {s[i], 0}; txtS(b, x + i * 6 * sz, y, wheel(off + i * 20), sz); }
}
bool hit(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}
void heart(int x, int y, uint16_t c) {
  cv.fillCircle(x + 2, y + 2, 2, c); cv.fillCircle(x + 5, y + 2, 2, c); cv.fillTriangle(x, y + 3, x + 7, y + 3, x + 3, y + 8, c);
}
void bevel(int x, int y, int w, int h, uint16_t c) {
  cv.fillRect(x, y, w, h, c);
  cv.drawFastHLine(x, y, w, mix(c, C_W, 140)); cv.drawFastVLine(x, y, h, mix(c, C_W, 90));
  cv.drawFastHLine(x, y + h - 1, w, mix(c, C_K, 150)); cv.drawFastVLine(x + w - 1, y, h, mix(c, C_K, 110));
}
void dim() { for (int i = 0; i < SW * SH; i++) FB[i] = (FB[i] >> 1) & 0x7BEF; }

// ---------- جسيمات ونجوم ----------
struct Pt { float x, y, vx, vy, life, mx; uint16_t c; };
Pt pts[72];
void burst(float x, float y, uint16_t c, int n = 10, float sp = 60, float ls = 1) {
  for (int k = 0; k < n; k++)
    for (auto &p : pts) if (p.life <= 0) {
      float a = random(628) / 100.0f, s = sp * (0.3f + random(100) / 100.0f);
      p.x = x; p.y = y; p.vx = cosf(a) * s; p.vy = sinf(a) * s; p.c = c;
      p.life = p.mx = (0.3f + random(40) / 100.0f) * ls; break;
    }
}
void updPts(float dt) { for (auto &p : pts) if (p.life > 0) { p.life -= dt; p.x += p.vx * dt; p.y += p.vy * dt; p.vy += 90 * dt; } }
void drawPts() {
  for (auto &p : pts) if (p.life > 0) {
    uint16_t c = mix(p.c, C_K, 255 - (uint8_t)(255 * p.life / p.mx));
    if (p.life > p.mx * 0.5f) cv.fillRect((int)p.x, (int)p.y, 2, 2, c); else cv.drawPixel((int)p.x, (int)p.y, c);
  }
}

struct Star { float x, y, s; };
Star stars[40];
void starsInit() { for (auto &s : stars) { s.x = random(SW); s.y = random(SH); s.s = 8 + random(70); } }
void starsDraw(float mult) {
  for (auto &s : stars) {
    s.y += s.s * gdt * mult; if (s.y >= SH) { s.y -= SH; s.x = random(SW); }
    int b = 60 + (int)(s.s * 2);
    cv.drawPixel(s.x, s.y, RGB(b, b, b));
    if (s.s > 60) cv.drawPixel(s.x, s.y - 1, RGB(b / 2, b / 2, b / 2));
  }
}


void readInput(float dt) {
  bool any = false;
  for (int i = 0; i < 8; i++) { bool d = digitalRead(PIN_BTN[i]) == LOW; rawP[i] = d && !rawH[i]; rawH[i] = d; if (d) any = true; }
  for (int a = 0; a < 8; a++) {
    int p = cfg.map[a]; held[a] = rawH[p]; prs[a] = rawP[p]; rpt[a] = false;
    if (prs[a]) { rpt[a] = true; holdT[a] = 0; }
    else if (held[a]) { holdT[a] += dt; if (holdT[a] > 0.4f) { rpt[a] = true; holdT[a] = 0.34f; } }
  }
  if (any) lastAct = millis();
}


const float VT[21] = {3.27, 3.61, 3.69, 3.71, 3.73, 3.75, 3.77, 3.79, 3.80, 3.82, 3.84, 3.85, 3.87, 3.91, 3.95, 3.98, 4.02, 4.08, 4.11, 4.15, 4.20};
const uint8_t PT[21] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95, 100};
int pctFromV(float v) {
  if (v <= VT[0]) return 0;
  for (int i = 1; i < 21; i++) if (v <= VT[i]) return PT[i - 1] + (int)((PT[i] - PT[i - 1]) * (v - VT[i - 1]) / (VT[i] - VT[i - 1]));
  return 100;
}
static void sortU(uint16_t *a, int n) { for (int i = 1; i < n; i++) { uint16_t k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; } }
static void sortF(float *a, int n) { for (int i = 1; i < n; i++) { float k = a[i]; int j = i - 1; while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; } a[j + 1] = k; } }

void readSensors() {
  uint32_t now = millis();
 
  {
    uint16_t sm[32];
    for (int i = 0; i < 32; i++) { sm[i] = analogReadMilliVolts(BATTERY_PIN); delayMicroseconds(120); }
    sortU(sm, 32);
    uint32_t s = 0; for (int i = 6; i < 26; i++) s += sm[i];
    float v = (s / 20.0f / 1000.0f) * cfg.vfac;
    static uint8_t bad = 0; static bool wasNo = false;
    if (!batInit) { batSm = v; batInit = true; bad = 0; batPct = (v < 2.5f) ? 0 : pctFromV(v); }
    else {
      float d = v - batSm; bool jump = fabsf(d) > 0.12f;
      if (jump) bad++; else bad = 0;
      if (!jump || bad >= 4) batSm = batSm + d * (jump ? 0.5f : 0.06f);
    }
    batV = batSm; batPinV = batSm / cfg.vfac;
    noBat = batSm < 2.5f;
    if (noBat) batPct = 0;
    else {
      int tg = pctFromV(batSm);
      if (wasNo) batPct = tg;
      int diff = tg - batPct;
      uint32_t gap = abs(diff) >= 8 ? 1000 : (diff < 0 ? 6000 : 8000);
      static uint32_t lastStep = 0;
      if (diff != 0 && now - lastStep >= gap) { batPct = batPct + (diff > 0 ? 1 : -1); lastStep = now; }
    }
    wasNo = noBat;
    static uint32_t tRef = 0; static float vRef = 0;
    if (now - tRef > 20000) {
      if (tRef) { float dv = batSm - vRef; if (dv > 0.02f) charging = true; else if (dv < -0.008f) charging = false; }
      vRef = batSm; tRef = now;
    }
    if (noBat) charging = false;
    if (!noBat) { if (batSm < 3.50f) lowBat = true; else if (batSm > 3.58f) lowBat = false; }
    static uint32_t lowSince = 0;
    if (!noBat && batSm < 3.25f) { if (!lowSince) lowSince = now; else if (now - lowSince > 8000) reqSleep = true; } else lowSince = 0;
  }
 
  {
    float tb[24];
    for (int i = 0; i < 24; i++) { tb[i] = temperatureRead(); delayMicroseconds(250); }
    sortF(tb, 24);
    float t = 0; for (int i = 6; i < 18; i++) t += tb[i]; t /= 12.0f;
    if (!tempInit) { tempRaw = t; tempInit = true; tempC = tempRaw - cfg.toff; tempShow = tempC; }
    else {
      float d = fabsf(t - tempRaw); float al = d > 3 ? 0.6f : (d > 1.2f ? 0.2f : 0.07f);
      tempRaw = tempRaw + (t - tempRaw) * al;
    }
    tempC = tempRaw - cfg.toff;
    if (fabsf(tempC - tempShow) >= 0.4f) tempShow = roundf(tempC * 2.0f) / 2.0f;
    if (tempRaw > 85) hot = true; else if (tempRaw < 80) hot = false;
    thermalDim = hot; if (tempRaw > 105) reqSleep = true;
  }
}

void applyBL() {
  int p = cfg.bl; if (thermalDim && p > 40) p = 40; if (idleDim && p > 15) p = 15;
  BL_WRITE((p * p * 255) / 10000);
}


void clearGRAM() {
  uint8_t c[4] = {0, 0, 0, 131}, r[4] = {0, 0, 0, 161};
  tft.sendCommand(ST77XX_CASET, c, 4); tft.sendCommand(ST77XX_RASET, r, 4); tft.sendCommand(ST77XX_RAMWR);
  tft.startWrite(); tft.writeColor(0, 132 * 162); tft.endWrite();
}
void applyTft() {
  xSemaphoreTake(semDone, portMAX_DELAY);
  tft.setOffset(cfg.colS, cfg.rowS); tft.setRotation(0); clearGRAM();
  xSemaphoreGive(semDone);
}
void tftTask(void *) {
  for (;;) {
    xSemaphoreTake(semGo, portMAX_DELAY);
    int sx = pSx, sy = pSy;
    tft.startWrite();
    if (sx || sy) {
      int x0 = sx > 0 ? sx : 0, y0 = sy > 0 ? sy : 0, w = SW - abs(sx), h = SH - abs(sy);
      tft.setAddrWindow(x0, y0, w, h);
      for (int y = (sy < 0 ? -sy : 0); y < SH - (sy > 0 ? sy : 0); y++) tft.writePixels(FB2 + y * SW + (sx < 0 ? -sx : 0), w, true, false);
    } else {
      tft.setAddrWindow(0, 0, SW, SH);
      tft.writePixels(FB2, SW * SH, true, false);
    }
    tft.endWrite();
    xSemaphoreGive(semDone);
    vTaskDelay(1);
  }
}
void present() {
  xSemaphoreTake(semDone, portMAX_DELAY);
  memcpy(FB2, FB, SW * SH * 2);
  if (shake > 0) { pSx = random(-2, 3); pSy = random(-2, 3); } else { pSx = 0; pSy = 0; }
  xSemaphoreGive(semGo);
}


#define SN_W 16
#define SN_H 14
int8_t snx[SN_W * SN_H], sny[SN_W * SN_H], spx[SN_W * SN_H], spy[SN_W * SN_H];
int snLen, sdx, sdy, ax, ay, turnQ[2], turnN;
float snT, snStep;
void snApple() {
  bool ok;
  do { ax = random(SN_W); ay = random(SN_H); ok = true;
    for (int i = 0; i < snLen; i++) if (snx[i] == ax && sny[i] == ay) { ok = false; break; }
  } while (!ok);
}
void snInit() {
  snLen = 3; for (int i = 0; i < 3; i++) { snx[i] = spx[i] = 8 - i; sny[i] = spy[i] = 7; }
  sdx = 1; sdy = 0; turnN = 0; snT = 0; snStep = 0.16f; snApple();
}
void snUpd(float dt) {
  if (prs[K_LF] && turnN < 2) turnQ[turnN++] = -1;
  if (prs[K_RT] && turnN < 2) turnQ[turnN++] = 1;
  snT += dt;
  if (snT < snStep) return;
  snT -= snStep;
  if (turnN) {
    int t = turnQ[0]; turnQ[0] = turnQ[1]; turnN--;
    int ndx, ndy;
    if (t < 0) { ndx = sdy; ndy = -sdx; } else { ndx = -sdy; ndy = sdx; }
    sdx = ndx; sdy = ndy; sndL(1200, 20);
  }
  int nx = snx[0] + sdx, ny = sny[0] + sdy;
  if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H) { burst(snx[0] * 8 + 4, 18 + sny[0] * 8, RGB(60, 255, 140), 20); die(); return; }
  for (int i = 0; i < snLen - 1; i++) if (snx[i] == nx && sny[i] == ny) { burst(nx * 8 + 4, 18 + ny * 8, RGB(60, 255, 140), 20); die(); return; }
  bool eat = (nx == ax && ny == ay);
  int oldLen = snLen;
  for (int i = 0; i < snLen; i++) { spx[i] = snx[i]; spy[i] = sny[i]; }
  if (eat && snLen < SN_W * SN_H) { snLen++; spx[oldLen] = snx[oldLen - 1]; spy[oldLen] = sny[oldLen - 1]; }
  for (int i = snLen - 1; i > 0; i--) { snx[i] = snx[i - 1]; sny[i] = sny[i - 1]; }
  snx[0] = nx; sny[0] = ny; spx[0] = snx[0] - sdx; spy[0] = sny[0] - sdy;
  if (eat) {
    score += 10; burst(nx * 8 + 4, 18 + ny * 8, RGB(255, 60, 80), 14); SFX(SFX_EAT);
    snStep = fmaxf(0.07f, snStep * 0.975f);
    if (snLen < SN_W * SN_H) snApple();
  }
}
void snDraw() {
  for (int y = 0; y < SN_H; y++) for (int x = 0; x < SN_W; x++)
    cv.fillRect(x * 8, 14 + y * 8, 8, 8, ((x + y) & 1) ? RGB(14, 24, 34) : RGB(10, 18, 28));
  cv.fillRect(0, 12, SW, 2, RGB(8, 14, 20));
  cv.drawRect(0, 13, 128, 114, RGB(0, 170, 120));
  float pl = sinf(millis() * 0.008f);
  int cx = ax * 8 + 4, cy = 14 + ay * 8 + 4;
  cv.drawCircle(cx, cy, 4 + (pl > 0), RGB(120, 20, 40));
  cv.fillCircle(cx, cy, 3, RGB(255, 40, 60)); cv.drawPixel(cx - 1, cy - 1, C_W); cv.drawPixel(cx + 1, cy - 3, RGB(80, 255, 80));
  float f = snT / snStep; if (f > 1) f = 1;
  for (int i = snLen - 1; i >= 0; i--) {
    int px_ = (int)((spx[i] + (snx[i] - spx[i]) * f) * 8), py_ = 14 + (int)((spy[i] + (sny[i] - spy[i]) * f) * 8);
    uint16_t c = mix(RGB(70, 255, 150), RGB(0, 100, 90), i * 255 / (snLen > 1 ? snLen : 1));
    cv.fillRoundRect(px_, py_, 8, 8, 3, c); cv.drawFastHLine(px_ + 2, py_ + 1, 4, mix(c, C_W, 120));
    if (i == 0) {
      int ex = px_ + 4, ey = py_ + 4;
      if (sdx) { cv.fillRect(ex + sdx, ey - 3, 2, 2, C_W); cv.fillRect(ex + sdx, ey + 1, 2, 2, C_W); }
      else { cv.fillRect(ex - 3, ey + sdy, 2, 2, C_W); cv.fillRect(ex + 1, ey + sdy, 2, 2, C_W); }
    }
  }
}


float bkx, bkbx, bkby, bkvx, bkvy, bkSp; bool bkStuck; uint8_t bkBr[5][8]; int bkLeft, bkLvl;
const uint16_t bkCol[5] = {RGB(255, 60, 90), RGB(255, 150, 40), RGB(255, 230, 60), RGB(70, 230, 110), RGB(60, 170, 255)};
void bkReset() { for (int r = 0; r < 5; r++) for (int c = 0; c < 8; c++) bkBr[r][c] = (r == 0) ? 2 : 1; bkLeft = 40; bkStuck = true; }
void bkInit() { lives = 3; bkLvl = 1; bkx = 64; bkSp = 95; bkReset(); }
void bkUpd(float dt) {
  if (held[K_LF]) bkx -= 120 * dt;
  if (held[K_RT]) bkx += 120 * dt;
  bkx = constrain(bkx, 15, 113);
  if (bkStuck) {
    bkbx = bkx; bkby = 114;
    if (prs[K_A] || prs[K_UP]) { bkStuck = false; bkvx = random(2) ? 40 : -40; bkvy = -sqrtf(bkSp * bkSp - 1600); snd(900, 40); }
    return;
  }
  bkbx += bkvx * dt; bkby += bkvy * dt;
  burst(bkbx, bkby, RGB(120, 160, 255), 1, 0, 0.35f);
  if (bkbx < 2) { bkbx = 2; bkvx = -bkvx; sndL(400, 24); }
  if (bkbx > 126) { bkbx = 126; bkvx = -bkvx; sndL(400, 24); }
  if (bkby < 14) { bkby = 14; bkvy = -bkvy; sndL(400, 24); }
  if (bkvy > 0 && bkby + 2 >= 118 && bkby <= 123 && fabsf(bkbx - bkx) < 15) {
    float off = (bkbx - bkx) / 15.0f;
    bkvx = off * bkSp * 0.8f; bkvy = -sqrtf(fmaxf(400, bkSp * bkSp - bkvx * bkvx));
    bkby = 117; snd(700, 30);
  }
  bool done = false;
  for (int r = 0; r < 5 && !done; r++) for (int c = 0; c < 8 && !done; c++) {
    if (!bkBr[r][c]) continue;
    float x = 4 + c * 15, y = 18 + r * 7;
    if (bkbx + 2 > x && bkbx - 2 < x + 14 && bkby + 2 > y && bkby - 2 < y + 6) {
      float ox = fminf(bkbx + 2 - x, x + 14 - (bkbx - 2)), oy = fminf(bkby + 2 - y, y + 6 - (bkby - 2));
      if (ox < oy) bkvx = -bkvx; else bkvy = -bkvy;
      if (--bkBr[r][c] == 0) { score += 10 * (5 - r); bkLeft--; burst(x + 7, y + 3, bkCol[r], 12); } else score += 5;
      snd(600 + r * 150, 35); done = true;
    }
  }
  if (bkLeft <= 0) { bkLvl++; bkSp = fminf(170, bkSp + 12); bkReset(); SFX(SFX_WIN); }
  if (bkby > 132) { lives--; shake = 0.2f; SFX(SFX_HIT); if (lives <= 0) die(); else bkStuck = true; }
}
void bkDraw() {
  vgrad(12, SH, RGB(10, 10, 40), RGB(35, 10, 55));
  for (int r = 0; r < 5; r++) for (int c = 0; c < 8; c++) if (bkBr[r][c])
    bevel(4 + c * 15, 18 + r * 7, 14, 6, bkBr[r][c] == 2 ? RGB(210, 220, 240) : bkCol[r]);
  cv.fillRoundRect((int)bkx - 13, 118, 26, 5, 2, RGB(190, 210, 255));
  cv.drawFastHLine((int)bkx - 11, 119, 22, C_W);
  cv.fillRoundRect((int)bkx - 13, 121, 26, 2, 1, RGB(90, 110, 200));
  cv.fillCircle((int)bkbx, (int)bkby, 2, C_W);
  if (bkStuck) txtC("A: LAUNCH", 100, RGB(255, 255, 120));
}

// ---------- 3) FLAPPY ----------
#define F_GAP 40
#define F_GROUND 112
float fy, fvy, fScroll, fpx[3], fgy[3], fspd; bool fStart, fpass[3];
void fInit() {
  fy = 60; fvy = 0; fStart = false; fScroll = 0; fspd = 50;
  for (int i = 0; i < 3; i++) { fpx[i] = 150 + i * 64; fgy[i] = random(26, 64); fpass[i] = false; }
}
void fUpd(float dt) {
  bool flap = prs[K_A] || prs[K_UP];
  if (!fStart) { fy = 60 + sinf(millis() * 0.006f) * 4; fScroll += 40 * dt; if (flap) { fStart = true; fvy = -150; SFX(SFX_JUMP); } return; }
  if (flap) { fvy = -150; SFX(SFX_JUMP); burst(32, fy + 4, C_W, 3, 25, 0.5f); }
  fvy += 480 * dt; fy += fvy * dt;
  fspd = 50 + fminf(score, 30) * 1.2f; fScroll += fspd * dt;
  for (int i = 0; i < 3; i++) {
    fpx[i] -= fspd * dt;
    if (fpx[i] < -22) { fpx[i] += 192; fgy[i] = random(26, 64); fpass[i] = false; }
    if (!fpass[i] && fpx[i] + 20 < 30) { fpass[i] = true; score++; snd(1400, 40); }
    if (34 + 4 > fpx[i] && 34 - 4 < fpx[i] + 20 && (fy - 3 < fgy[i] || fy + 3 > fgy[i] + F_GAP)) { burst(34, fy, RGB(255, 220, 40), 18); die(); return; }
  }
  if (fy + 4 >= F_GROUND) { fy = F_GROUND - 4; burst(34, fy, RGB(255, 220, 40), 18); die(); return; }
  if (fy < 14) { fy = 14; fvy = 0; }
}
void fDraw() {
  vgrad(12, F_GROUND, RGB(70, 190, 250), RGB(200, 238, 255));
  for (int x = 0; x < SW; x++) {
    int h1 = 14 + (int)(8 * sinf((x + fScroll * 0.2f) * 0.06f) + 5 * sinf((x + fScroll * 0.2f) * 0.15f));
    cv.drawFastVLine(x, F_GROUND - h1, h1, RGB(120, 200, 190));
    int h2 = 6 + (int)(5 * sinf((x + fScroll * 0.5f) * 0.09f) + 3 * sinf((x + fScroll * 0.5f) * 0.23f));
    cv.drawFastVLine(x, F_GROUND - h2, h2, RGB(70, 170, 120));
  }
  for (int i = 0; i < 4; i++) {
    int cx = ((int)(i * 52 - fScroll * 0.3f) % 208 + 208) % 208 - 40, cy = 24 + (i * 17) % 40;
    cv.fillCircle(cx, cy, 6, C_W); cv.fillCircle(cx + 7, cy + 2, 5, C_W); cv.fillCircle(cx - 7, cy + 2, 4, C_W); cv.fillRect(cx - 8, cy + 2, 16, 5, C_W);
  }
  for (int i = 0; i < 3; i++) {
    int x = (int)fpx[i], g = (int)fgy[i];
    uint16_t gc = RGB(90, 200, 60), gl = RGB(170, 240, 110), gd = RGB(40, 120, 40);
    cv.fillRect(x, 12, 20, g - 12 - 6, gc); cv.fillRect(x + 2, 12, 3, g - 18, gl); cv.fillRect(x + 16, 12, 4, g - 18, gd);
    cv.fillRect(x - 2, g - 7, 24, 7, gc); cv.drawRect(x - 2, g - 7, 24, 7, gd); cv.drawFastHLine(x, g - 6, 20, gl);
    int b = g + F_GAP;
    cv.fillRect(x, b + 7, 20, F_GROUND - b - 7, gc); cv.fillRect(x + 2, b + 7, 3, F_GROUND - b - 7, gl); cv.fillRect(x + 16, b + 7, 4, F_GROUND - b - 7, gd);
    cv.fillRect(x - 2, b, 24, 7, gc); cv.drawRect(x - 2, b, 24, 7, gd); cv.drawFastHLine(x, b + 1, 20, gl);
  }
  cv.fillRect(0, F_GROUND, SW, 16, RGB(222, 200, 130));
  cv.fillRect(0, F_GROUND, SW, 3, RGB(90, 200, 60));
  for (int x = -16; x < SW; x += 8) cv.drawLine(x - (int)fScroll % 8 + 8, F_GROUND + 3, x - (int)fScroll % 8, F_GROUND + 6, RGB(60, 150, 40));
  int by = (int)fy;
  cv.fillCircle(34, by, 5, RGB(255, 215, 40)); cv.fillCircle(33, by + 2, 3, RGB(255, 240, 150));
  int wy = fvy < 0 ? -2 : 1;
  cv.fillCircle(31, by + wy, 2, RGB(255, 140, 20));
  cv.fillCircle(37, by - 2, 2, C_W); cv.drawPixel(38, by - 2, C_K);
  cv.fillTriangle(38, by, 42, by + 1, 38, by + 3, RGB(255, 90, 40));
  if (!fStart) txtC("A: FLAP", 84, RGB(255, 255, 255));
}


#define RGY 100
#define RB 12
#define RPX 28
#define RCEIL 24
#define RSPD 115.0f
#define GD_BGT RGB(24, 120, 255)
#define GD_BGB RGB(8, 62, 205)

const char RLV[] =
  "............"
  "^.......^.......^^......^^^........"
  "#.......##......$$$.......#...#...."
  "p.^^^^^.........p.^^^^^.........."
  "....___.......___........___O___....."
  "S.........u.........d.........uu........dd........u.....d.....uu......D.......U.......u....d....."
  "C.........^....^....#.....^^.....$$.....^........"
  "p.^^^^^.....o......^^^.......___.....^.^.^......"
  "$$$$.....___O___....^^......#.....$.....$......p.^^^^^......^^^..."
  "...............W............";
int RLEN, rFinish; bool rZone[520], rused[520];
float rpy, rpvy, rrot, rdist, rdead, rpad; bool rgrounded, rship; int rattempt;
char rtok(int c) { return (c < 0 || c >= RLEN) ? '.' : RLV[c]; }
int rBlockH(char t) { switch (t) { case '#': return 1; case '$': return 2; case '%': return 3; case 'd': return 3; case 'D': return 4; case 'u': return 3; case 'U': return 4; } return 0; }
void rRun() {
  rpy = RGY - RB; rpvy = 0; rrot = 0; rdist = 0; rdead = 0; rgrounded = true; rship = false; rpad = 0; score = 0;
  memset(rused, 0, sizeof(rused)); for (auto &p : pts) p.life = 0;
}
void rInit() {
  RLEN = strlen(RLV); if (RLEN > 519) RLEN = 519;
  rFinish = 0; for (int i = 0; i < RLEN; i++) if (RLV[i] == 'W') rFinish = i;
  bool z = false; for (int i = 0; i < RLEN; i++) { if (RLV[i] == 'S') z = true; rZone[i] = z; if (RLV[i] == 'C') z = false; }
  rattempt = 1; rRun();
}
void rKill() {
  if (rdead > 0) return;
  rdead = 0.9f;
  burst(RPX + 6, rpy + 6, RGB(120, 255, 0), 22, 110, 1.3f); burst(RPX + 6, rpy + 6, RGB(0, 235, 255), 14, 90, 1.2f); burst(RPX + 6, rpy + 6, C_K, 8, 80, 1.2f);
  SFX(SFX_DIE);
}
void rUpd(float dt) {
  if (rpad > 0) rpad -= dt;
  if (rdead > 0) { rdead -= dt; if (rdead <= 0) { rattempt++; rRun(); } return; }
  rdist += RSPD * dt;
  float wx = rdist + RPX;
  float fin = rFinish * RB - RPX;
  score = constrain((int)(rdist * 100.0f / fin), 0, 100);
  bool jh = held[K_A] || held[K_UP], jp = prs[K_A] || prs[K_UP];
  int c0 = (int)((wx + RB / 2) / RB); char tk = rtok(c0);
  if (tk == 'S' && !rship) { rship = true; snd(900, 120); }
  if (tk == 'C' && rship) { rship = false; snd(600, 120); }
  if (tk == 'W') { score = 100; won = true; die(); return; }
  float prevBottom = rpy + RB;
  if (!rship) {
    if (rgrounded && jh) { rpvy = -290; rgrounded = false; SFX(SFX_JUMP); }
    rpvy += 1200 * dt; rpy += rpvy * dt;
  } else {
    rpvy += (jh ? -1300 : 950) * dt; rpvy = constrain(rpvy, -190, 190); rpy += rpvy * dt;
    if (rpy < RCEIL) { rpy = RCEIL; if (rpvy < 0) rpvy = 0; }
    if (rpy + RB > RGY) { rpy = RGY - RB; if (rpvy > 0) rpvy = 0; }
  }
  rgrounded = false;
  int ca = (int)((wx + 1) / RB), cb = (int)((wx + RB - 1) / RB);
  bool gapAll = true;
  for (int c = ca; c <= cb; c++) {
    char t = rtok(c); int bh = rBlockH(t);
    if (t != '_' && t != 'O') gapAll = false;
    if (!rship) {
      if (bh > 0 && t != 'u' && t != 'U') {
        float top = RGY - bh * RB;
        if (prevBottom <= top + 3 && rpy + RB >= top) { rpy = top - RB; rpvy = 0; rgrounded = true; }
        else if (rpy + RB > top + 3) { rKill(); return; }
      }
    } else {
      if (t == 'u' || t == 'U') { if (rpy < RCEIL + bh * RB) { rKill(); return; } }
      else if (bh > 0) { if (rpy + RB > RGY - bh * RB + 1) { rKill(); return; } }
    }
    if (t == '^') {
      float sx = c * RB - rdist + 3, sw = RB - 6;
      if (RPX + 2 < sx + sw && RPX + RB - 2 > sx && rpy + RB - 2 > RGY - 8) { rKill(); return; }
    }
    if (t == 'p' && !rship && rpy + RB >= RGY - 1) { rpvy = -450; rgrounded = false; rpad = 0.25f; snd(1100, 80); burst(RPX + 6, RGY, RGB(255, 255, 0), 8, 70); }
  }
  for (int c = ca - 1; c <= cb + 1; c++) {
    char t = rtok(c);
    if ((t == 'o' || t == 'O') && jp && !rused[c] && !rship) {
      float ox = c * RB + RB / 2.0f - rdist, oy = RGY - 2.4f * RB;
      if (fabsf(RPX + RB / 2.0f - ox) < 11 && fabsf(rpy + RB / 2.0f - oy) < 13) {
        rused[c] = true; rpvy = -290; rgrounded = false; snd(1300, 70); burst(ox, oy, RGB(255, 255, 0), 10, 70);
      }
    }
  }
  if (!rship && rpy + RB >= RGY && !gapAll && rpvy >= 0) { rpy = RGY - RB; rpvy = 0; rgrounded = true; }
  if (rpy > SH + 10) { rKill(); return; }
  if (!rship) { if (rgrounded) rrot = roundf(rrot / (PI / 2)) * (PI / 2); else rrot += dt * 6.5f; }
  if ((millis() & 63) < 20) {
    if (rship) burst(RPX - 4, rpy + 6, RGB(255, 170, 40), 1, 25, 0.4f);
    else if (rgrounded) burst(RPX, RGY, RGB(120, 220, 255), 1, 20, 0.4f);
  }
}
void rotSq(float cx, float cy, float half, float a, uint16_t c) {
  float s = sinf(a), co = cosf(a), pxs[4], pys[4];
  const int sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; i++) { pxs[i] = cx + (sx[i] * co - sy[i] * s) * half; pys[i] = cy + (sx[i] * s + sy[i] * co) * half; }
  cv.fillTriangle(pxs[0], pys[0], pxs[1], pys[1], pxs[2], pys[2], c); cv.fillTriangle(pxs[0], pys[0], pxs[2], pys[2], pxs[3], pys[3], c);
}
void rotRect(float cx, float cy, float hw, float hhh, float a, uint16_t c) {
  float s = sinf(a), co = cosf(a), pxs[4], pys[4];
  const int sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
  for (int i = 0; i < 4; i++) { pxs[i] = cx + sx[i] * hw * co - sy[i] * hhh * s; pys[i] = cy + sx[i] * hw * s + sy[i] * hhh * co; }
  cv.fillTriangle(pxs[0], pys[0], pxs[1], pys[1], pxs[2], pys[2], c); cv.fillTriangle(pxs[0], pys[0], pxs[2], pys[2], pxs[3], pys[3], c);
}
// بلوك GD الأصلي: جسم أسود + حد أبيض + إطار داخلي خافت
void gdBlock(int x, int y, int w, int h) {
  cv.fillRect(x, y, w, h, C_K); cv.drawRect(x, y, w, h, C_W);
  if (w > 6 && h > 6) cv.drawRect(x + 2, y + 2, w - 4, h - 4, RGB(60, 90, 160));
}
void gdOrb(int c, int x) {
  float ox = x + RB / 2.0f, oy = RGY - 2.4f * RB; bool u = (c >= 0 && c < 520) ? rused[c] : false;
  uint16_t oc = u ? RGB(110, 110, 40) : RGB(255, 255, 0);
  cv.drawCircle(ox, oy, 6, oc); cv.drawCircle(ox, oy, 5, oc); cv.fillCircle(ox, oy, 2, u ? RGB(160, 160, 100) : C_W);
}
void rDraw() {
 
  vgrad(0, RGY, GD_BGT, GD_BGB);
  int off = (int)(rdist * 0.15f), ts = 26;
  for (int j = 0; j * ts < RGY; j++) for (int i = -1; i <= SW / ts + 1; i++) {
    int idx = i + off / ts + 64;
    if (hh(idx * 7 + j * 13) & 1) {
      int x = i * ts - off % ts, y = j * ts;
      uint16_t c = mix(mix(GD_BGT, GD_BGB, (uint8_t)constrain(y * 255 / RGY, 0, 255)), C_W, 14);
      cv.fillRect(x, y, ts - 2, ts - 2, c);
    }
  }
  // الأرض
  cv.fillRect(0, RGY, SW, SH - RGY, RGB(4, 52, 175));
  int gs = (int)rdist % RB;
  for (int x = -gs; x < SW; x += RB) cv.drawFastVLine(x, RGY + 2, SH - RGY, RGB(2, 36, 130));
  for (int y = RGY + RB; y < SH; y += RB) cv.drawFastHLine(0, y, SW, RGB(2, 36, 130));
  cv.fillRect(0, RGY, SW, 2, C_W); cv.drawFastHLine(0, RGY + 2, SW, RGB(150, 205, 255));
  int rd = (int)rdist, cA = rd / RB;
  for (int c = cA; c <= cA + SW / RB + 2; c++) {
    char t = rtok(c); int x = c * RB - rd;
    if (c >= 0 && c < RLEN && rZone[c]) {   // سقف مرحلة السفينة (أسود بحد أبيض)
      cv.fillRect(x, 0, RB, RCEIL, C_K); cv.drawFastHLine(x, RCEIL, RB, C_W); cv.drawRect(x, RCEIL - RB, RB, RB, RGB(60, 90, 160));
    }
    switch (t) {
      case '^':
        cv.fillTriangle(x + 1, RGY, x + RB / 2, RGY - RB + 1, x + RB - 1, RGY, C_K);
        cv.drawTriangle(x + 1, RGY, x + RB / 2, RGY - RB + 1, x + RB - 1, RGY, C_W); break;
      case '#': case '$': case '%': case 'd': case 'D':
        for (int k = 0; k < rBlockH(t); k++) gdBlock(x, RGY - (k + 1) * RB, RB, RB); break;
      case 'u': case 'U':
        for (int k = 0; k < rBlockH(t); k++) gdBlock(x, RCEIL + k * RB, RB, RB); break;
      case '_': case 'O':
        cv.fillRect(x, RGY, RB, SH - RGY, GD_BGB); cv.drawFastVLine(x, RGY, SH - RGY, C_W); cv.drawFastVLine(x + RB - 1, RGY, SH - RGY, C_W);
        if (t == 'O') gdOrb(c, x);
        break;
      case 'p':
        cv.fillRoundRect(x + 1, RGY - 4, RB - 2, 4, 2, RGB(255, 255, 0)); cv.drawFastHLine(x + 3, RGY - 4, RB - 6, RGB(255, 255, 170)); break;
      case 'o': gdOrb(c, x); break;
      case 'S': case 'C': {
        uint16_t pc = (t == 'S') ? RGB(255, 60, 200) : RGB(60, 255, 90);
        cv.drawRoundRect(x + 2, RCEIL, 8, RGY - RCEIL, 4, pc); cv.drawRoundRect(x + 4, RCEIL + 4, 4, RGY - RCEIL - 8, 2, mix(pc, C_W, 120)); break; }
      case 'W':
        for (int k = 0; k < 10; k++) for (int j = 0; j < 2; j++) cv.fillRect(x + j * 6, k * 10, 6, 10, ((k + j) & 1) ? C_W : C_K);
        break;
    }
  }
  // اللاعب
  if (rdead <= 0) {
    float cx = RPX + RB / 2.0f, cy = rpy + RB / 2.0f;
    if (!rship) {
      float sq = rpad > 0 ? 0.9f : 1.0f, s = sinf(rrot), co = cosf(rrot);
      rotSq(cx, cy, 5.4f * sq, rrot, C_K);
      rotSq(cx, cy, 4.6f * sq, rrot, RGB(120, 255, 0));
      rotSq(cx, cy, 3.0f * sq, rrot, RGB(0, 235, 255));
      // وش المكعب (عينين + فم)
      for (int e = -1; e <= 1; e += 2) { float lx = e * 1.4f, ly = -0.8f; px((int)(cx + lx * co - ly * s), (int)(cy + lx * s + ly * co), C_K); }
      for (int m = -1; m <= 1; m++) { float lx = m * 1.2f, ly = 1.3f; px((int)(cx + lx * co - ly * s), (int)(cy + lx * s + ly * co), C_K); }
    } else {
      float a = atan2f(rpvy, RSPD), s = sinf(a), co = cosf(a);
      float p[3][2] = {{10, 0}, {-8, -5}, {-8, 5}};
      int q[3][2]; for (int i = 0; i < 3; i++) { q[i][0] = (int)(cx + p[i][0] * co - p[i][1] * s); q[i][1] = (int)(cy + p[i][0] * s + p[i][1] * co); }
      cv.fillTriangle(q[0][0], q[0][1], q[1][0], q[1][1], q[2][0], q[2][1], RGB(120, 255, 0));
      cv.drawTriangle(q[0][0], q[0][1], q[1][0], q[1][1], q[2][0], q[2][1], C_K);
      cv.fillCircle((int)cx, (int)cy - 1, 3, RGB(0, 235, 255)); cv.drawCircle((int)cx, (int)cy - 1, 3, C_K);
      if (held[K_A] || held[K_UP]) cv.fillTriangle((int)(cx - 9), (int)cy - 2, (int)(cx - 9), (int)cy + 2, (int)(cx - 15 - random(3)), (int)cy, RGB(255, 170, 30));
    }
  }
  // شريط التقدم (زي GD)
  float prog = fminf(1.0f, rdist / (rFinish * RB - RPX));
  cv.fillRoundRect(6, 3, 86, 7, 3, C_K); cv.drawRoundRect(6, 3, 86, 7, 3, C_W);
  int bw = (int)(80 * prog); if (bw > 0) cv.fillRoundRect(9, 5, bw, 3, 1, RGB(120, 255, 0));
  char b[16]; snprintf(b, 16, "%d%%", score); txtS(b, 96, 2, C_W);
  if (rdist < 260 && rdead <= 0) {
    snprintf(b, 16, "ATTEMPT %d", rattempt);
    cv.setTextSize(1); cv.setTextColor(C_W); cv.setCursor(40 - (int)rdist, 44); cv.print(b);
  }
}

// ---------- 5) STARFIRE (مراحل + بوس + شيلد + تطوير سلاح + قنابل) ----------
struct Bl { float x, y, vx, vy; bool on; };
Bl pb[28], eb[48];
struct En { float x, y, x0, ph, t, cd, vx, vy; int8_t hp; uint8_t ty; bool on; };
En en[10];
struct PUp { float x, y; uint8_t k; bool on; };
PUp pup[4];
struct Boss { float x, y, t, cd, cd2, ang, flash, dieT; int hp, mhp; uint8_t ph; bool on, dying; } bs;
float shx, shy, shCd, shInv, shSpawn, shBanner, shFlash;
int shWpn, shBombs, shSh, shStage, shKills, shGoal;
char shMsg[16] = "";
const uint16_t PUC[4] = {RGB(255, 160, 40), RGB(0, 255, 255), RGB(255, 90, 150), RGB(255, 230, 80)};
void shBannerSet(const char *s, float t) { snprintf(shMsg, sizeof(shMsg), "%s", s); shBanner = t; }
void shInit() {
  shx = 64; shy = 108; lives = 3; shCd = 0; shInv = 0; shSpawn = 1; shWpn = 1; shBombs = 1; shSh = 0; shStage = 1; shKills = 0; shGoal = 14; shFlash = 0;
  memset(&bs, 0, sizeof(bs));
  for (auto &b : pb) b.on = false; for (auto &b : eb) b.on = false; for (auto &e : en) e.on = false; for (auto &p : pup) p.on = false;
  shBannerSet("STAGE 1", 2.0f);
}
void shFire(float x, float y, float vx) { for (auto &b : pb) if (!b.on) { b.on = true; b.x = x; b.y = y; b.vx = vx; b.vy = -200; break; } }
void ebFire(float x, float y, float ang, float sp) { for (auto &b : eb) if (!b.on) { b.on = true; b.x = x; b.y = y; b.vx = cosf(ang) * sp; b.vy = sinf(ang) * sp; break; } }
float aimAng(float x, float y) { return atan2f(shy - y, shx - x); }
void shDrop(float x, float y, int chance) {
  if (random(100) >= chance) return;
  for (auto &p : pup) if (!p.on) { p.on = true; p.x = x; p.y = y; int r = random(100); p.k = r < 40 ? 0 : (r < 65 ? 1 : (r < 80 ? 2 : 3)); break; }
}
void shHurt() {
  if (shInv > 0) return;
  if (shSh > 0) { shSh--; shInv = 0.6f; burst(shx, shy, RGB(0, 255, 255), 12, 70); SFX(SFX_HIT); return; }
  lives--; if (shWpn > 1) shWpn--; shInv = 1.8f; shake = 0.25f; burst(shx, shy, RGB(80, 180, 255), 22, 80); SFX(SFX_BOOM);
  if (lives <= 0) die();
}
void bsDmg(int d) {
  if (!bs.on || bs.dying) return;
  bs.hp -= d; bs.flash = 0.07f; score += d;
  if (bs.hp <= 0) { bs.dying = true; bs.dieT = 1.8f; for (auto &b : eb) b.on = false; SFX(SFX_BOOM); shake = 0.5f; }
}
void shBomb() {
  if (shBombs <= 0) return;
  shBombs--; shFlash = 0.35f; shake = 0.3f; SFX(SFX_BOOM);
  for (auto &b : eb) b.on = false;
  for (auto &e : en) if (e.on) { e.on = false; score += 10; burst(e.x, e.y, RGB(255, 200, 80), 10, 70); }
  bsDmg(18);
}
void shSpawnEn(bool minion) {
  for (auto &e : en) if (!e.on) {
    int r = random(100); uint8_t ty = r < 58 ? 0 : (r < 85 ? 1 : 2);
    if (shStage < 2 && ty == 2) ty = 0; if (minion) ty = 0;
    e.on = true; e.ty = ty; e.hp = ty == 1 ? 3 : (ty == 2 ? 2 : 1); e.x0 = minion ? bs.x : random(16, 112); e.x = e.x0; e.y = minion ? bs.y : -6;
    e.t = 0; e.ph = random(100) / 15.0f; e.cd = 1.0f + random(100) / 70.0f; e.vx = e.vy = 0; break;
  }
}
void shUpd(float dt) {
  if (shBanner > 0) shBanner -= dt;
  if (shFlash > 0) shFlash -= dt;
  shx += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 88 * dt; shy += ((held[K_DN] ? 1 : 0) - (held[K_UP] ? 1 : 0)) * 88 * dt;
  shx = constrain(shx, 8, 120); shy = constrain(shy, 40, 118);
  shCd -= dt; shInv -= dt;
  if (prs[K_B]) shBomb();
  if (held[K_A] && shCd <= 0) {
    shCd = shWpn >= 4 ? 0.14f : 0.17f;
    switch (shWpn) {
      case 1: shFire(shx, shy - 8, 0); break;
      case 2: shFire(shx - 4, shy - 8, 0); shFire(shx + 4, shy - 8, 0); break;
      case 3: shFire(shx, shy - 8, 0); shFire(shx - 4, shy - 6, -35); shFire(shx + 4, shy - 6, 35); break;
      case 4: shFire(shx - 4, shy - 8, 0); shFire(shx + 4, shy - 8, 0); shFire(shx - 6, shy - 6, -40); shFire(shx + 6, shy - 6, 40); break;
      default: for (int k = -2; k <= 2; k++) shFire(shx + k * 2, shy - 8, k * 30.0f); break;
    }
    SFXL(SFX_LASER);
  }
  for (auto &b : pb) if (b.on) { b.x += b.vx * dt; b.y += b.vy * dt; if (b.y < 12 || b.x < -4 || b.x > 132) b.on = false; }
  // ظهور الأعداء / البوس
  if (!bs.on && shKills >= shGoal) {
    bs.on = true; bs.x = 64; bs.y = -24; bs.t = 0; bs.mhp = bs.hp = 70 + shStage * 35; bs.ph = 0; bs.cd = 1.5f; bs.cd2 = 0; bs.ang = 0; bs.flash = 0; bs.dying = false;
    shBannerSet("WARNING", 2.2f); SFX(SFX_GROWL);
  }
  if (!bs.on) {
    shSpawn -= dt;
    if (shSpawn <= 0) { shSpawn = fmaxf(0.45f, 1.2f - shStage * 0.05f - score * 0.0002f); shSpawnEn(false); }
  }
  for (auto &e : en) if (e.on) {
    e.t += dt;
    if (e.ty == 0) { e.y += (34 + shStage * 3) * dt; e.x = e.x0 + sinf(e.t * 2.4f + e.ph) * 22; }
    else if (e.ty == 1) {
      e.y += 20 * dt; e.x = e.x0 + sinf(e.t * 1.4f + e.ph) * 12; e.cd -= dt;
      if (e.cd <= 0 && e.y > 8 && e.y < 95) { e.cd = fmaxf(0.9f, 1.5f - shStage * 0.1f); ebFire(e.x, e.y + 6, aimAng(e.x, e.y), 60 + shStage * 4); }
    } else {
      if (e.t < 1.2f) { if (e.y < 26) e.y += 40 * dt; e.x = e.x0 + sinf(e.t * 3) * 3; }
      else {
        if (e.vx == 0 && e.vy == 0) { float a = aimAng(e.x, e.y); e.vx = cosf(a) * 105; e.vy = sinf(a) * 105; }
        e.x += e.vx * dt; e.y += e.vy * dt; if ((millis() & 31) < 10) burst(e.x, e.y - 3, RGB(255, 120, 40), 1, 15, 0.3f);
      }
    }
    if (e.y > 136 || e.x < -14 || e.x > 142) { e.on = false; continue; }
    float ew = e.ty == 1 ? 12 : 10, eh = e.ty == 1 ? 10 : 8;
    for (auto &b : pb) if (b.on && hit(b.x - 1, b.y - 3, 2, 5, e.x - ew / 2, e.y - eh / 2, ew, eh)) {
      b.on = false; e.hp--; burst(b.x, b.y, RGB(255, 220, 80), 4, 40); sndL(500, 24);
      if (e.hp <= 0) {
        e.on = false; score += e.ty == 1 ? 30 : (e.ty == 2 ? 20 : 10); shKills++;
        burst(e.x, e.y, e.ty == 1 ? RGB(190, 90, 255) : RGB(255, 80, 90), 20, 80); SFX(SFX_HIT);
        shDrop(e.x, e.y, e.ty == 1 ? 28 : 9);
      }
      break;
    }
    if (e.on && hit(shx - 6, shy - 7, 12, 14, e.x - ew / 2, e.y - eh / 2, ew, eh)) { e.on = false; burst(e.x, e.y, RGB(255, 120, 60), 16, 70); shHurt(); if (over) return; }
  }
  // البوس
  if (bs.on) {
    bs.t += dt; bs.flash -= dt;
    if (bs.dying) {
      bs.dieT -= dt; if (((int)(bs.dieT * 20)) & 1) burst(bs.x + random(-18, 19), bs.y + random(-8, 9), RGB(255, 160, 60), 6, 70);
      if (bs.dieT <= 0) {
        bs.on = false; score += 500 * shStage; shStage++; shKills = 0; shGoal = 12 + shStage * 4;
        char b[16]; snprintf(b, 16, "STAGE %d", shStage); shBannerSet(b, 2.4f);
        shDrop(bs.x - 10, bs.y, 100); shDrop(bs.x + 10, bs.y, 100);
        if (lives < 5) lives++; if (shBombs < 3) shBombs++; SFX(SFX_WIN);
      }
    } else {
      float tyy = 28;
      float frac = (float)bs.hp / bs.mhp; bs.ph = frac > 0.66f ? 0 : (frac > 0.33f ? 1 : 2);
      if (bs.y < tyy) bs.y += 40 * dt;
      else {
        bs.x = 64 + sinf(bs.t * 0.8f * (1.0f + bs.ph * 0.5f)) * 34;
        bs.cd -= dt;
        if (bs.ph == 0 && bs.cd <= 0) { bs.cd = 1.1f; float a = aimAng(bs.x, bs.y + 10); for (int k = -1; k <= 1; k++) ebFire(bs.x, bs.y + 12, a + k * 0.28f, 70); sndL(500, 40); }
        if (bs.ph == 1) {
          if (bs.cd <= 0) { bs.cd = 1.7f; for (int k = 0; k < 12; k++) ebFire(bs.x, bs.y + 8, k * PI / 6 + bs.t, 52); sndL(420, 60); }
          bs.cd2 -= dt; if (bs.cd2 <= 0) { bs.cd2 = 0.6f; ebFire(bs.x, bs.y + 12, aimAng(bs.x, bs.y), 85); }
        }
        if (bs.ph == 2) {
          bs.cd2 -= dt; if (bs.cd2 <= 0) { bs.cd2 = 0.11f; bs.ang += 0.55f; ebFire(bs.x, bs.y + 8, bs.ang, 58); ebFire(bs.x, bs.y + 8, bs.ang + PI, 58); }
          if (bs.cd <= 0) { bs.cd = 3.2f; shSpawnEn(true); shSpawnEn(true); }
        }
      }
      for (auto &b : pb) if (b.on && hit(b.x - 1, b.y - 3, 2, 5, bs.x - 22, bs.y - 12, 44, 24)) { b.on = false; burst(b.x, b.y, RGB(255, 220, 80), 3, 40); bsDmg(1); sndL(520, 20); }
      if (hit(shx - 6, shy - 7, 12, 14, bs.x - 22, bs.y - 12, 44, 24)) shHurt();
      if (over) return;
    }
  }
  for (auto &b : eb) if (b.on) {
    b.x += b.vx * dt; b.y += b.vy * dt; if (b.y > 132 || b.y < 8 || b.x < -4 || b.x > 132) b.on = false;
    if (b.on && hit(shx - 5, shy - 6, 10, 12, b.x - 2, b.y - 2, 4, 4)) { b.on = false; shHurt(); if (over) return; }
  }
  for (auto &p : pup) if (p.on) {
    p.y += 32 * dt; if (p.y > 132) { p.on = false; continue; }
    if (hit(shx - 7, shy - 8, 14, 16, p.x - 5, p.y - 5, 10, 10)) {
      p.on = false; SFX(SFX_POWER);
      if (p.k == 0) { if (shWpn < 5) shWpn++; else score += 50; }
      else if (p.k == 1) shSh = 3;
      else if (p.k == 2) { if (lives < 5) lives++; else score += 50; }
      else { if (shBombs < 3) shBombs++; else score += 50; }
    }
  }
}
void shDraw() {
  static const uint16_t BG0[4] = {RGB(2, 2, 15), RGB(12, 2, 18), RGB(2, 12, 18), RGB(16, 8, 2)};
  static const uint16_t BG1[4] = {RGB(22, 8, 42), RGB(48, 8, 40), RGB(8, 40, 60), RGB(60, 28, 10)};
  int pi = (shStage - 1) & 3; vgrad(12, SH, BG0[pi], BG1[pi]);
  starsDraw(1.0f);
  for (auto &b : pb) if (b.on) { cv.fillRect((int)b.x - 1, (int)b.y - 3, 2, 6, RGB(255, 240, 100)); cv.drawPixel((int)b.x, (int)b.y - 4, C_W); }
  for (auto &b : eb) if (b.on) { cv.fillCircle((int)b.x, (int)b.y, 2, RGB(255, 120, 0)); cv.drawPixel((int)b.x, (int)b.y, RGB(255, 255, 160)); }
  for (auto &e : en) if (e.on) {
    int x = (int)e.x, y = (int)e.y;
    if (e.ty == 0) { cv.fillCircle(x, y, 4, RGB(255, 70, 90)); cv.fillRect(x - 6, y - 1, 12, 3, RGB(180, 30, 60)); cv.drawPixel(x, y - 1, C_W); cv.drawPixel(x - 4, y + 3, RGB(255, 200, 80)); cv.drawPixel(x + 4, y + 3, RGB(255, 200, 80)); }
    else if (e.ty == 1) { cv.fillRoundRect(x - 6, y - 5, 12, 10, 3, RGB(170, 70, 255)); cv.fillRect(x - 3, y - 2, 6, 4, RGB(70, 20, 120)); cv.drawPixel(x, y, RGB(255, 80, 80)); cv.drawFastHLine(x - 5, y - 4, 10, RGB(220, 160, 255)); }
    else {
      bool bl = e.t > 0.8f && e.t < 1.2f && ((int)(e.t * 20) & 1);
      cv.fillTriangle(x - 5, y - 4, x + 5, y - 4, x, y + 6, bl ? C_W : RGB(255, 140, 40)); cv.fillTriangle(x - 3, y - 4, x + 3, y - 4, x, y + 2, RGB(120, 30, 20));
    }
  }
  if (bs.on) {
    int x = (int)bs.x, y = (int)bs.y; bool fl = bs.flash > 0 || (bs.dying && ((millis() / 60) & 1));
    uint16_t body = fl ? C_W : RGB(150, 40, 210), dk = fl ? RGB(220, 200, 255) : RGB(70, 16, 110);
    cv.fillTriangle(x - 30, y - 4, x - 42, y + 12, x - 22, y + 4, dk); cv.fillTriangle(x + 30, y - 4, x + 42, y + 12, x + 22, y + 4, dk);
    cv.fillRoundRect(x - 30, y - 4, 60, 8, 3, dk);
    cv.fillRoundRect(x - 22, y - 10, 44, 20, 6, body); cv.drawRoundRect(x - 22, y - 10, 44, 20, 6, mix(body, C_W, 120));
    cv.fillRect(x - 14, y + 8, 6, 6, dk); cv.fillRect(x + 8, y + 8, 6, 6, dk);
    cv.fillCircle(x, y, 6, RGB(30, 6, 50)); cv.fillCircle(x, y, 3, bs.ph ? RGB(255, 60, 60) : RGB(0, 255, 255));
    if (!bs.dying) {
      cv.drawRect(13, 21, 102, 5, C_W); int w = (int)(100.0f * bs.hp / bs.mhp);
      cv.fillRect(14, 22, w, 3, bs.ph == 0 ? RGB(80, 255, 120) : (bs.ph == 1 ? RGB(255, 220, 60) : RGB(255, 60, 60)));
    }
  }
  for (auto &p : pup) if (p.on) {
    int x = (int)p.x, y = (int)p.y, r = 5 + ((millis() / 150) & 1);
    cv.fillCircle(x, y, 5, RGB(10, 12, 30)); cv.drawCircle(x, y, r, PUC[p.k]);
    char c[2] = {"WSHB"[p.k], 0}; txt(c, x - 2, y - 3, PUC[p.k]);
  }
  if (!(shInv > 0 && ((int)(shInv * 12) & 1))) {
    int x = (int)shx, y = (int)shy;
    cv.fillTriangle(x - 2, y + 7, x + 2, y + 7, x, y + 10 + random(4), RGB(255, 150, 30));
    cv.fillTriangle(x, y - 8, x - 6, y + 6, x + 6, y + 6, RGB(70, 170, 255));
    cv.fillTriangle(x - 6, y + 6, x - 10, y + 8, x - 5, y, RGB(40, 100, 200));
    cv.fillTriangle(x + 6, y + 6, x + 10, y + 8, x + 5, y, RGB(40, 100, 200));
    cv.fillCircle(x, y - 1, 2, RGB(0, 255, 255));
    if (shSh > 0) { cv.drawCircle(x, y, 11, RGB(0, 255, 255)); if (millis() & 64) cv.drawCircle(x, y, 12, RGB(0, 120, 160)); }
  }
  for (int i = 0; i < shWpn; i++) cv.fillRect(3 + i * 4, 14, 3, 3, RGB(255, 160, 40));
  for (int i = 0; i < shBombs; i++) cv.fillCircle(40 + i * 7, 15, 2, RGB(255, 230, 80));
  for (int i = 0; i < shSh; i++) cv.drawCircle(70 + i * 7, 15, 2, RGB(0, 255, 255));
  char b[8]; snprintf(b, 8, "ST%d", shStage); txt(b, 102, 14, RGB(180, 180, 255));
  if (shBanner > 0 && ((int)(shBanner * 6) & 1) == 0) txtC(shMsg, 50, shMsg[0] == 'W' ? RGB(255, 60, 60) : RGB(255, 255, 120), 2);
  if (shFlash > 0) for (int y = 12; y < SH; y += 2) hl(0, SW - 1, y, mix(FB[y * SW], C_W, 190));
}

// ---------- 6) TETRIS ----------
const int8_t TP[7][4][2] = {
  {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, {{0, 1}, {1, 1}, {2, 1}, {1, 0}},
  {{1, 0}, {2, 0}, {0, 1}, {1, 1}}, {{0, 0}, {1, 0}, {1, 1}, {2, 1}}, {{0, 0}, {0, 1}, {1, 1}, {2, 1}}, {{2, 0}, {0, 1}, {1, 1}, {2, 1}}};
const uint8_t TN[7] = {4, 2, 3, 3, 3, 3, 3};
const uint16_t TC[8] = {0, RGB(0, 230, 240), RGB(250, 220, 40), RGB(180, 70, 240), RGB(70, 220, 90), RGB(240, 60, 70), RGB(60, 110, 250), RGB(250, 150, 40)};
uint8_t tb[20][10]; int tType, tRot, tX, tY, tNext, tLines, tLvl; float tFall, tL, tR;
void tCells(int ty, int rot, int out[4][2]) {
  int n = TN[ty];
  for (int i = 0; i < 4; i++) { int x = TP[ty][i][0], y = TP[ty][i][1]; for (int r = 0; r < rot; r++) { int nx = n - 1 - y, ny = x; x = nx; y = ny; } out[i][0] = x; out[i][1] = y; }
}
bool tFits(int ty, int rot, int pxx, int pyy) {
  int c[4][2]; tCells(ty, rot, c);
  for (int i = 0; i < 4; i++) { int x = pxx + c[i][0], y = pyy + c[i][1]; if (x < 0 || x >= 10 || y >= 20) return false; if (y >= 0 && tb[y][x]) return false; }
  return true;
}
void tSpawn() { tType = tNext; tNext = random(7); tRot = 0; tX = 3; tY = 0; tFall = 0; if (!tFits(tType, tRot, tX, tY)) die(); }
void tInit() { memset(tb, 0, sizeof(tb)); tLines = 0; tLvl = 0; tL = tR = 0; tNext = random(7); tSpawn(); }
void tLock() {
  int c[4][2]; tCells(tType, tRot, c);
  for (int i = 0; i < 4; i++) { int x = tX + c[i][0], y = tY + c[i][1]; if (y >= 0) tb[y][x] = tType + 1; }
  int cl = 0;
  for (int y = 19; y >= 0; y--) {
    bool full = true; for (int x = 0; x < 10; x++) if (!tb[y][x]) { full = false; break; }
    if (full) {
      for (int x = 0; x < 10; x++) burst(8 + x * 6 + 3, 5 + y * 6 + 3, TC[tb[y][x]], 2, 70);
      for (int yy = y; yy > 0; yy--) memcpy(tb[yy], tb[yy - 1], 10); memset(tb[0], 0, 10); cl++; y++;
    }
  }
  static const int pts2[5] = {0, 100, 300, 500, 800};
  if (cl) { score += pts2[cl] * (tLvl + 1); tLines += cl; tLvl = tLines / 10; shake = 0.1f * cl; snd(1000 + cl * 300, 120); } else sndL(250, 40);
  tSpawn();
}
void tUpd(float dt) {
  if (prs[K_LF]) { if (tFits(tType, tRot, tX - 1, tY)) tX--; tL = 0; }
  else if (held[K_LF]) { tL += dt; if (tL > 0.17f) { tL -= 0.05f; if (tFits(tType, tRot, tX - 1, tY)) tX--; } }
  if (prs[K_RT]) { if (tFits(tType, tRot, tX + 1, tY)) tX++; tR = 0; }
  else if (held[K_RT]) { tR += dt; if (tR > 0.17f) { tR -= 0.05f; if (tFits(tType, tRot, tX + 1, tY)) tX++; } }
  if (prs[K_A]) {
    int nr = (tRot + 1) & 3; static const int kk[5] = {0, -1, 1, -2, 2};
    for (int i = 0; i < 5; i++) if (tFits(tType, nr, tX + kk[i], tY)) { tRot = nr; tX += kk[i]; sndL(900, 24); break; }
  }
  if (prs[K_UP] || prs[K_B]) { while (tFits(tType, tRot, tX, tY + 1)) { tY++; score += 2; } shake = 0.08f; tLock(); return; }
  tFall += dt; float iv = held[K_DN] ? 0.035f : fmaxf(0.06f, 0.8f - tLvl * 0.07f);
  if (tFall >= iv) { tFall = 0; if (tFits(tType, tRot, tX, tY + 1)) { tY++; if (held[K_DN]) score++; } else tLock(); }
}
void tBlk(int x, int y, uint16_t c) { bevel(x, y, 6, 6, c); }
void tDraw() {
  vgrad(0, SH, RGB(6, 8, 24), RGB(18, 8, 36));
  cv.fillRect(6, 3, 64, 122, RGB(60, 70, 120)); cv.fillRect(7, 4, 62, 120, RGB(8, 10, 24));
  for (int y = 0; y < 20; y++) for (int x = 0; x < 10; x++) {
    int pxx = 8 + x * 6, pyy = 5 + y * 6;
    if (tb[y][x]) tBlk(pxx, pyy, TC[tb[y][x]]); else cv.drawPixel(pxx + 3, pyy + 3, RGB(28, 34, 66));
  }
  int c[4][2]; tCells(tType, tRot, c);
  int gy = tY; while (tFits(tType, tRot, tX, gy + 1)) gy++;
  for (int i = 0; i < 4; i++) { int x = tX + c[i][0]; cv.drawRect(8 + x * 6, 5 + (gy + c[i][1]) * 6, 6, 6, mix(TC[tType + 1], C_K, 120)); }
  for (int i = 0; i < 4; i++) { int y = tY + c[i][1]; if (y >= 0) tBlk(8 + (tX + c[i][0]) * 6, 5 + y * 6, TC[tType + 1]); }
  txtS("NEXT", 80, 5, RGB(0, 220, 255));
  cv.drawRoundRect(76, 15, 48, 32, 4, RGB(60, 70, 120));
  int nc[4][2]; tCells(tNext, 0, nc); int n = TN[tNext];
  for (int i = 0; i < 4; i++) tBlk(100 - n * 3 + nc[i][0] * 6, 25 + nc[i][1] * 6, TC[tNext + 1]);
  char b[16];
  txtS("SCORE", 80, 54, RGB(0, 220, 255)); snprintf(b, 16, "%d", score); txtS(b, 80, 64, C_W);
  txtS("LINES", 80, 80, RGB(0, 220, 255)); snprintf(b, 16, "%d", tLines); txtS(b, 80, 90, C_W);
  txtS("LEVEL", 80, 104, RGB(0, 220, 255)); snprintf(b, 16, "%d", tLvl + 1); txtS(b, 80, 114, C_W);
}

// ---------- 7) PONG ----------
float pgP, pgA, pbx, pby, pvx, pvy, pSp, pServe;
void pgServe(int dir) { pbx = 64; pby = 70; pSp = 85; float a = random(-30, 31) * 0.01f; pvx = dir * pSp * cosf(a); pvy = pSp * sinf(a); pServe = 0.9f; }
void pgInit() { lives = 3; pgP = pgA = 70; pgServe(1); }
void pgUpd(float dt) {
  if (held[K_UP]) pgP -= 105 * dt;
  if (held[K_DN]) pgP += 105 * dt;
  pgP = constrain(pgP, 25, 115);
  float tgt = pvx > 0 ? pby : 70, sp = fminf(100, 55 + score * 0.6f);
  if (fabsf(tgt - pgA) > 3) pgA += (tgt > pgA ? 1 : -1) * sp * dt;
  pgA = constrain(pgA, 25, 115);
  if (pServe > 0) { pServe -= dt; return; }
  pbx += pvx * dt; pby += pvy * dt; burst(pbx, pby, RGB(0, 255, 200), 1, 0, 0.3f);
  if (pby < 16) { pby = 16; pvy = -pvy; sndL(500, 24); }
  if (pby > 124) { pby = 124; pvy = -pvy; sndL(500, 24); }
  if (pvx < 0 && pbx - 2 <= 8 && pbx > 0 && fabsf(pby - pgP) < 12) {
    pbx = 10; pSp = fminf(200, pSp * 1.05f); float a = (pby - pgP) / 12.0f * 0.9f; pvx = pSp * cosf(a); pvy = pSp * sinf(a); snd(800, 30); burst(pbx, pby, RGB(0, 255, 200), 6, 50);
  }
  if (pvx > 0 && pbx + 2 >= 120 && pbx < 128 && fabsf(pby - pgA) < 12) {
    pbx = 118; pSp = fminf(200, pSp * 1.03f); float a = (pby - pgA) / 12.0f * 0.9f; pvx = -pSp * cosf(a); pvy = pSp * sinf(a); snd(600, 30); burst(pbx, pby, RGB(255, 80, 160), 6, 50);
  }
  if (pbx < -4) { lives--; shake = 0.2f; SFX(SFX_HIT); if (lives <= 0) die(); else pgServe(1); }
  if (pbx > 132) { score += 10; SFX(SFX_EAT); pgServe(-1); }
}
void pgDraw() {
  vgrad(12, SH, RGB(4, 12, 26), RGB(14, 6, 34));
  cv.drawFastHLine(0, 13, SW, RGB(0, 200, 255)); cv.drawFastHLine(0, 126, SW, RGB(0, 200, 255));
  for (int y = 16; y < 124; y += 8) cv.fillRect(63, y, 2, 4, RGB(50, 70, 120));
  cv.fillRoundRect(4, (int)pgP - 10, 4, 20, 2, RGB(0, 255, 200)); cv.drawFastVLine(5, (int)pgP - 8, 16, C_W);
  cv.fillRoundRect(120, (int)pgA - 10, 4, 20, 2, RGB(255, 80, 160)); cv.drawFastVLine(121, (int)pgA - 8, 16, C_W);
  cv.fillCircle((int)pbx, (int)pby, 2, C_W);
}

// ---------- 8) RACER 3D (جراج + عملات + BMW M3 GTR + طريق ثلاثي الأبعاد + منحنيات + مرور + نيترو) ----------
#define RC_HOR 50
#define RC_H (SH - RC_HOR)
#define RC_CAM 380.0f
#define RC_HZ rcFog
uint16_t rcFog = RGB(255, 100, 150); int rcZone = 0; float rcZoneT = 0;
float rcPz, rcSpd, rcPx, rcNit, rcBg, rcInv, rcSteer, rcDist, rcScoreF;
RcCar rcCars[7]; RcCoin rcCn[8];
float rcCx[RC_H + 2], rcHw[RC_H + 2];
const uint16_t RC_COL[6] = {RGB(60, 200, 255), RGB(255, 220, 60), RGB(120, 255, 120), RGB(200, 120, 255), RGB(255, 255, 255), RGB(255, 140, 40)};
// العربيات: الاسم، لون الجسم، غامق، إضافي، السعر، سرعة، تسارع، تحكم، نيترو، شكل
const CarSpec CARS[6] = {
  {"HATCH",      RGB(255, 40, 120),  RGB(200, 20, 90),   RGB(0, 255, 255),   0,    1.00f, 1.00f, 1.00f, 1.00f, 0},
  {"COUPE",      RGB(255, 210, 40),  RGB(200, 150, 20),  RGB(30, 30, 50),    150,  1.06f, 1.05f, 1.05f, 1.05f, 1},
  {"MUSCLE",     RGB(255, 120, 30),  RGB(190, 70, 10),   RGB(240, 240, 250), 350,  1.12f, 1.15f, 0.92f, 1.00f, 2},
  {"SUPER",      RGB(50, 150, 255),  RGB(20, 90, 200),   RGB(255, 255, 255), 700,  1.16f, 1.20f, 1.10f, 1.20f, 3},
  {"BMW M3 GTR", RGB(130, 155, 195), RGB(52, 66, 100),   RGB(255, 200, 40),  1500, 1.24f, 1.35f, 1.20f, 1.40f, 4},
  {"HYPERCAR",   RGB(200, 60, 255),  RGB(120, 20, 170),  RGB(0, 255, 200),   3000, 1.32f, 1.40f, 1.30f, 1.60f, 5}};
uint32_t rcCoins = 0, rcRunCoins = 0; uint8_t rcOwn = 1, rcSel = 0; int rcMode = 0, rcBrowse = 0; bool rcBanked = true; float rcMsgT = 0; const char *rcMsg = "";
static uint16_t carBuf[44 * 30];
static inline float rcCurve(float z) { return 1.1f * sinf(z * 0.0070f) + 0.7f * sinf(z * 0.0173f + 1.3f); }
void rcSpawn(RcCar &c, float zmin) {
  c.on = true; c.pass = false; c.z = rcPz + zmin + random(0, 120); c.lx = (random(3) - 1) * 0.62f;
  c.sp = 12 + random(0, 12); c.col = RC_COL[random(6)];
}
void rcCoinGroup(int g, float zmin) {
  float lane = (random(3) - 1) * 0.62f, zb = rcPz + zmin + random(0, 80);
  for (int k = 0; k < 4; k++) { RcCoin &c = rcCn[g * 4 + k]; c.z = zb + k * 13; c.lx = lane; c.on = true; }
}
void rcSaveData() { pref.putUInt("coins", rcCoins); pref.putUChar("own", rcOwn); pref.putUChar("car", rcSel); }
void rcBank() { if (rcBanked) return; rcBanked = true; rcCoins += rcRunCoins + score / 50; rcRunCoins = 0; rcSaveData(); }
void rcLeave() { if (rcMode == 1) rcBank(); }
void rcInit() {
  rcCoins = pref.getUInt("coins", 0); rcOwn = pref.getUChar("own", 1) | 1; rcSel = pref.getUChar("car", 0);
  if (rcSel > 5 || !(rcOwn & (1 << rcSel))) rcSel = 0;
  rcBrowse = rcSel; rcMode = 0; lives = 0; rcMsgT = 0; rcBanked = true; rcRunCoins = 0;
}
void rcStart() {
  lives = 3; rcPz = 0; rcSpd = 22; rcPx = 0; rcNit = 1; rcBg = 0; rcInv = 0; rcSteer = 0; rcDist = 0; rcScoreF = 0; score = 0;
  for (int k = 0; k < 7; k++) rcSpawn(rcCars[k], 40 + k * 40);
  rcCoinGroup(0, 70); rcCoinGroup(1, 190);
  rcMode = 1; rcBanked = false; rcRunCoins = 0; rcZone = 0; rcZoneT = 2.5f;
}
// ---- رسم عربية اللاعب من الخلف (حسب الموديل) ----
void rcPlayerCar(int cx, int oy, int lean, const CarSpec &c, bool brake, bool nit) {
  int bw = (c.kind >= 2) ? 17 : 15; if (c.kind == 4) bw = 18;
  cv.fillRoundRect(cx - bw - 2, 121 + oy, bw * 2 + 4, 4, 2, RGB(8, 4, 20));
  cv.fillRect(cx - bw - 1, 112 + oy, 6, 10, C_K); cv.fillRect(cx + bw - 5, 112 + oy, 6, 10, C_K);
  cv.fillRoundRect(cx - bw + lean, 106 + oy, bw * 2, 12, 3, c.body);
  int cw2 = (c.kind == 3 || c.kind == 5) ? 16 : 20;
  cv.fillRoundRect(cx - cw2 / 2 + lean, 98 + oy, cw2, 10, 3, c.dark);
  cv.fillRect(cx - cw2 / 2 + 2 + lean, 100 + oy, cw2 - 4, 6, RGB(30, 10, 60)); cv.drawFastHLine(cx - cw2 / 2 + 2 + lean, 100 + oy, cw2 - 4, RGB(120, 100, 200));
  if (c.kind == 2) { cv.fillRect(cx - 2 + lean, 106 + oy, 4, 12, c.accent); cv.fillRect(cx - 2 + lean, 99 + oy, 4, 8, c.accent); }
  uint16_t wing = (c.kind == 4) ? C_K : c.accent;
  if (c.kind == 0) { cv.fillRect(cx - 16 + lean, 101 + oy, 32, 3, wing); cv.fillRect(cx - 12 + lean, 103 + oy, 2, 4, c.dark); cv.fillRect(cx + 10 + lean, 103 + oy, 2, 4, c.dark); }
  else if (c.kind >= 3) {
    cv.fillRect(cx - bw - 2 + lean, 100 + oy, bw * 2 + 4, 3, wing); cv.drawFastHLine(cx - bw - 2 + lean, 100 + oy, bw * 2 + 4, RGB(150, 150, 160));
    cv.fillRect(cx - bw - 2 + lean, 98 + oy, 2, 7, wing); cv.fillRect(cx + bw + lean, 98 + oy, 2, 7, wing);
    cv.fillRect(cx - 9 + lean, 103 + oy, 2, 4, c.dark); cv.fillRect(cx + 7 + lean, 103 + oy, 2, 4, c.dark);
  } else cv.fillRect(cx - 9 + lean, 104 + oy, 18, 2, c.dark);
  uint16_t tl = brake ? RGB(255, 120, 120) : RGB(255, 20, 40);
  if (c.kind == 4) {   // BMW: طفايات دائرية + دائرة BMW + شرايط M
    cv.fillCircle(cx - bw + 5 + lean, 111 + oy, 2, tl); cv.fillCircle(cx - bw + 9 + lean, 111 + oy, 2, tl);
    cv.fillCircle(cx + bw - 6 + lean, 111 + oy, 2, tl); cv.fillCircle(cx + bw - 10 + lean, 111 + oy, 2, tl);
    cv.fillCircle(cx + lean, 110 + oy, 3, C_W); cv.fillRect(cx - 2 + lean, 108 + oy, 2, 2, RGB(30, 100, 220)); cv.fillRect(cx + lean, 110 + oy, 2, 2, RGB(30, 100, 220));
    cv.fillRect(cx - 9 + lean, 115 + oy, 4, 2, RGB(90, 180, 255)); cv.fillRect(cx - 5 + lean, 115 + oy, 4, 2, RGB(30, 50, 160)); cv.fillRect(cx - 1 + lean, 115 + oy, 4, 2, RGB(220, 30, 40));
  } else { cv.fillRect(cx - 14 + lean, 109 + oy, 6, 3, tl); cv.fillRect(cx + 8 + lean, 109 + oy, 6, 3, tl); }
  if (c.kind >= 3) { cv.fillRect(cx - 9 + lean, 118 + oy, 3, 2, RGB(70, 70, 80)); cv.fillRect(cx + 6 + lean, 118 + oy, 3, 2, RGB(70, 70, 80)); }
  if (nit) { cv.fillTriangle(cx - 6, 118 + oy, cx - 2, 118 + oy, cx - 4, 126 + oy + random(3), RGB(120, 200, 255)); cv.fillTriangle(cx + 2, 118 + oy, cx + 6, 118 + oy, cx + 4, 126 + oy + random(3), RGB(120, 200, 255)); }
}
// ---- شاشة الجراج ----
void rcSelUpd(float dt) {
  noPause = true; if (rcMsgT > 0) rcMsgT -= dt;
  if (rpt[K_LF] && rcBrowse > 0) { rcBrowse--; SFX(SFX_MOVE); }
  if (rpt[K_RT] && rcBrowse < 5) { rcBrowse++; SFX(SFX_MOVE); }
  if (prs[K_A] || prs[K_ST]) {
    if (rcOwn & (1 << rcBrowse)) { rcSel = rcBrowse; rcSaveData(); SFX(SFX_OK); rcStart(); }
    else if (rcCoins >= CARS[rcBrowse].price) { rcCoins -= CARS[rcBrowse].price; rcOwn |= (1 << rcBrowse); rcSaveData(); SFX(SFX_POWER); rcMsg = "UNLOCKED!"; rcMsgT = 1.5f; }
    else { SFX(SFX_HIT); rcMsg = "NEED MORE COINS"; rcMsgT = 1.5f; }
  }
}
void rcBar(int x, int y, const char *l, float v) {
  txt(l, x, y, RGB(150, 160, 210));
  cv.drawRect(x + 14, y, 40, 7, RGB(80, 90, 150));
  float n = constrain((v - 0.9f) / 0.75f, 0.0f, 1.0f); cv.fillRect(x + 15, y + 1, (int)(38 * n), 5, mix(RGB(0, 200, 255), RGB(255, 80, 160), (uint8_t)(n * 255)));
}
void rcSelDraw() {
  const CarSpec &c = CARS[rcBrowse];
  cv.fillRect(0, 0, 44, 30, 0xF81F);
  rcPlayerCar(22, -97, (int)(sinf(millis() * 0.002f) * 3), c, false, false);
  for (int y = 0; y < 30; y++) for (int x = 0; x < 44; x++) carBuf[y * 44 + x] = FB[y * SW + x];
  vgrad(0, SH, RGB(8, 6, 26), RGB(30, 10, 50));
  cv.fillRect(0, 0, SW, 13, RGB(10, 12, 34)); cv.drawFastHLine(0, 13, SW, RGB(0, 200, 255));
  txt("GARAGE", 4, 3, C_W); char b[24]; snprintf(b, 24, "$%u", (unsigned)rcCoins); txt(b, SW - 4 - strlen(b) * 6, 3, RGB(255, 220, 60));
  cv.fillRoundRect(18, 82, 92, 10, 5, RGB(34, 26, 78)); cv.drawRoundRect(18, 82, 92, 10, 5, RGB(0, 200, 255));
  for (int y = 0; y < 30; y++) for (int x = 0; x < 44; x++) {
    uint16_t pc = carBuf[y * 44 + x]; if (pc == 0xF81F) continue;
    int X = 24 + x * 2, Y = 28 + y * 2; px(X, Y, pc); px(X + 1, Y, pc); px(X, Y + 1, pc); px(X + 1, Y + 1, pc);
  }
  txtS(c.nm, (SW - (int)strlen(c.nm) * 6) / 2, 17, c.kind == 4 ? RGB(255, 220, 80) : C_W);
  txt("<", 5, 52, RGB(200, 200, 255), 2); txt(">", 111, 52, RGB(200, 200, 255), 2);
  if (rcMsgT > 0) txtC(rcMsg, 95, RGB(255, 255, 120));
  else if (rcOwn & (1 << rcBrowse)) txtC(rcBrowse == rcSel ? "SELECTED" : "OWNED", 95, RGB(120, 255, 160));
  else { snprintf(b, 24, "PRICE $%u", (unsigned)c.price); txtC(b, 95, rcCoins >= c.price ? RGB(255, 220, 60) : RGB(255, 100, 100)); }
  rcBar(4, 104, "SP", c.spd); rcBar(66, 104, "AC", c.acc); rcBar(4, 112, "HN", c.hnd); rcBar(66, 112, "NO", c.nit);
  txtC("A:RACE/BUY  SEL:MENU", 120, RGB(120, 130, 190));
}
void rcUpd(float dt) {
  if (rcMode == 0) { rcSelUpd(dt); return; }
  const CarSpec &cs = CARS[rcSel];
  float maxSp = (52 + fminf(18, rcDist * 0.004f)) * cs.spd;
  bool nit = held[K_A] && rcNit > 0.02f, brk = held[K_B];
  float target = brk ? 14 : (nit ? maxSp * 1.45f : maxSp);
  if (rcSpd < target) rcSpd += (nit ? 40 : 22) * cs.acc * dt; else rcSpd -= (brk ? 55 : 18) * dt;
  if (fabsf(rcPx) > 1.0f) { if (rcSpd > 24) rcSpd -= 45 * dt; if ((millis() & 31) < 10) burst(64 + rcPx * 10, 118, RGB(160, 140, 200), 1, 30, 0.4f); }
  if (nit) rcNit = fmaxf(0, rcNit - 0.35f / cs.nit * dt); else rcNit = fminf(1, rcNit + 0.04f * cs.nit * dt);
  float st = (held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0);
  rcSteer += (st - rcSteer) * fminf(1, dt * 16);
  rcPx += rcSteer * dt * (0.8f + rcSpd * 0.026f) * cs.hnd * sensM() * 0.8f;
  float c0 = rcCurve(rcPz + 8);
  rcPx -= c0 * rcSpd * 0.0085f * dt; rcPx = constrain(rcPx, -1.7f, 1.7f);
  { int z = ((int)(rcDist / 1500.0f)) % 3; if (z != rcZone) { rcZone = z; rcZoneT = 2.5f; } if (rcZoneT > 0) rcZoneT -= dt; }
  rcPz += rcSpd * dt; rcDist += rcSpd * dt; rcScoreF += rcSpd * dt * 0.1f; score = (int)rcScoreF;
  rcBg += c0 * rcSpd * dt * 0.08f; if (rcInv > 0) rcInv -= dt;
  if (nit && (millis() & 31) < 12) burst(64 + random(-8, 9), 122, RGB(120, 200, 255), 1, 40, 0.3f);
  for (auto &c : rcCars) {
    c.z += c.sp * dt; float rel = c.z - rcPz;
    if (rel < -12) { rcSpawn(c, 330); continue; }
    if (rcInv <= 0 && rel > 4.5f && rel < 8.5f && fabsf(c.lx - rcPx) < 0.30f) {
      rcInv = 1.6f; lives--; rcSpd *= 0.35f; shake = 0.3f; burst(64 + (c.lx - rcPx) * 20, 108, RGB(255, 200, 80), 20, 100); SFX(SFX_BOOM);
      rcSpawn(c, 330); if (lives <= 0) { rcBank(); die(); return; } continue;
    }
    if (!c.pass && rel < 4.5f) { c.pass = true; if (fabsf(c.lx - rcPx) < 0.6f && rcInv <= 0) { rcScoreF += 25; rcNit = fminf(1, rcNit + 0.12f); sndL(1300, 60); } }
  }
  for (int g = 0; g < 2; g++) {
    bool allPassed = true;
    for (int k = 0; k < 4; k++) {
      RcCoin &c = rcCn[g * 4 + k]; float rel = c.z - rcPz; if (rel > -12) allPassed = false;
      if (c.on && rel > 3.5f && rel < 9.0f && fabsf(c.lx - rcPx) < 0.34f) { c.on = false; rcRunCoins++; rcNit = fminf(1, rcNit + 0.03f); sndL(1500, 40); }
    }
    if (allPassed) rcCoinGroup(g, 200);
  }
}
void rcCarDraw(int i, float lx, uint16_t col) {
  if (i < 2 || i >= RC_H) return;
  float hw = rcHw[i]; int cx = (int)(rcCx[i] + lx * hw), y = RC_HOR + i;
  int w = (int)(hw * 0.34f) + 2, h = w * 11 / 20 + 2, x = cx - w / 2;
  uint8_t fa = (uint8_t)((RC_H - i) * 200 / RC_H);
  uint16_t c = mix(col, RC_HZ, fa), d = mix(mix(col, C_K, 120), RC_HZ, fa), k = mix(C_K, RC_HZ, fa);
  cv.fillRect(x - 1, y - 1, w + 2, 2, k);
  cv.fillRect(x, y - h, w, h - 1, c);
  cv.fillRect(x + w / 6, y - h - h / 2, w - w / 3, h / 2 + 1, d);
  cv.fillRect(x + w / 5, y - h - h / 2 + 1, w - 2 * (w / 5), (h / 3 > 1 ? h / 3 : 1), mix(RGB(20, 10, 50), RC_HZ, fa));
  int tl = w / 5 > 1 ? w / 5 : 1;
  cv.fillRect(x + 1, y - h + 1, tl, tl, RGB(255, 30, 30)); cv.fillRect(x + w - 1 - tl, y - h + 1, tl, tl, RGB(255, 30, 30));
  cv.fillRect(x - 1, y - h / 3 - 1, 2, h / 3 + 1, k); cv.fillRect(x + w - 1, y - h / 3 - 1, 2, h / 3 + 1, k);
}
const uint16_t ZSKY[3] = {RGB(52, 130, 235), RGB(70, 120, 200), RGB(12, 6, 48)};
const uint16_t ZFOG[3] = {RGB(190, 225, 250), RGB(200, 210, 225), RGB(255, 100, 150)};
const uint16_t ZGRA[3] = {RGB(72, 165, 62), RGB(112, 112, 118), RGB(30, 10, 80)}, ZGRB[3] = {RGB(62, 148, 54), RGB(100, 100, 106), RGB(20, 8, 60)};
const uint16_t ZROA[3] = {RGB(74, 74, 80), RGB(52, 52, 58), RGB(46, 42, 78)}, ZROB[3] = {RGB(66, 66, 72), RGB(44, 44, 50), RGB(38, 34, 66)};
const uint16_t ZRUA[3] = {RGB(220, 40, 40), RGB(230, 200, 60), RGB(255, 40, 120)}, ZRUB[3] = {RGB(245, 245, 245), RGB(60, 60, 66), RGB(240, 240, 255)};
const char *ZNAME[3] = {"COUNTRYSIDE", "DOWNTOWN", "NEON NIGHT"};
void rcCow(int sx, int sy, int ph, bool left, uint8_t fa) {
  int w = ph * 13 / 10 + 3, h = ph * 6 / 10 + 2, lg = ph * 3 / 10 + 1;
  uint16_t wht = mix(RGB(240, 240, 240), rcFog, fa), blk = mix(RGB(30, 30, 34), rcFog, fa), pnk = mix(RGB(240, 160, 170), rcFog, fa);
  cv.fillRect(sx - w / 2, sy - h - lg, w, h, wht);
  cv.fillRect(sx - w / 2 + w / 5, sy - h - lg + 1, w / 4 + 1, h / 2, blk); cv.fillRect(sx + w / 10, sy - h - lg + h / 3, w / 4 + 1, h / 2, blk);
  cv.fillRect(sx - w / 2 + 1, sy - lg, 2, lg, blk); cv.fillRect(sx + w / 2 - 3, sy - lg, 2, lg, blk);
  int hx = left ? sx - w / 2 - ph / 4 - 1 : sx + w / 2; cv.fillRect(hx, sy - h - lg - ph / 8, ph / 4 + 2, h * 3 / 4 + 1, wht);
  cv.fillRect(hx, sy - h - lg + h / 4, ph / 4 + 2, 2, pnk);
}
void rcTree(int sx, int sy, int ph, uint8_t fa) {
  cv.fillRect(sx - 1, sy - ph / 2, ph / 6 + 1, ph / 2, mix(RGB(90, 60, 30), rcFog, fa));
  cv.fillCircle(sx, sy - ph * 3 / 4, ph / 2 + 1, mix(RGB(40, 130, 50), rcFog, fa)); cv.fillCircle(sx - ph / 5, sy - ph * 2 / 3, ph / 3, mix(RGB(60, 160, 60), rcFog, fa));
}
void rcBuilding(int sx, int sy, int ph, uint32_t hv, uint8_t fa) {
  int w = ph * 11 / 10 + 4, h = ph * (13 + (int)((hv >> 3) & 7) * 3) / 10 + 4;
  static const uint16_t BC[4] = {RGB(110, 120, 150), RGB(150, 130, 120), RGB(90, 100, 120), RGB(140, 145, 155)};
  uint16_t c = mix(BC[hv & 3], rcFog, fa), win = mix(RGB(250, 225, 130), rcFog, fa), dk = mix(RGB(40, 50, 70), rcFog, fa);
  cv.fillRect(sx - w / 2, sy - h, w, h, c); cv.fillRect(sx - w / 2, sy - h, w, 2, mix(RGB(60, 60, 70), rcFog, fa));
  for (int wy = sy - h + 4; wy < sy - 3; wy += 4) for (int wx = sx - w / 2 + 2; wx < sx + w / 2 - 2; wx += 4) cv.fillRect(wx, wy, 2, 2, (hh(wx * 3 + wy * 7 + hv) & 3) ? dk : win);
}
void rcDraw() {
  if (rcMode == 0) { rcSelDraw(); return; }
  int zn = rcZone; rcFog = ZFOG[zn];
  vgrad(12, RC_HOR + 1, ZSKY[zn], rcFog);
  if (zn == 0) {   // ريف: شمس + سحاب + تلال
    int sunx = 100 - (int)(rcBg * 0.15f) % 260; if (sunx < -20) sunx += 260;
    cv.fillCircle(sunx, 24, 10, RGB(255, 246, 170)); cv.fillCircle(sunx, 24, 7, RGB(255, 255, 220));
    for (int k = 0; k < 5; k++) { int cx = ((k * 47 - (int)(rcBg * 0.3f)) % 235 + 235) % 235 - 30, cy = 16 + (k * 13) % 24;
      cv.fillCircle(cx, cy, 6, C_W); cv.fillCircle(cx + 7, cy + 2, 5, C_W); cv.fillCircle(cx - 6, cy + 2, 4, C_W); cv.fillRect(cx - 8, cy + 2, 16, 4, C_W); }
    for (int x = 0; x < SW; x++) {
      float u = x + rcBg; int h1 = 8 + (int)(7 * sinf(u * 0.035f) + 4 * sinf(u * 0.09f + 1)), h2 = 3 + (int)(4 * sinf(u * 0.06f + 2) + 2 * sinf(u * 0.17f));
      if (h1 > 0) cv.drawFastVLine(x, RC_HOR - h1, h1, RGB(120, 175, 135)); if (h2 > 0) cv.drawFastVLine(x, RC_HOR - h2, h2, RGB(72, 150, 72));
    }
  } else if (zn == 1) {   // مدينة: ناطحات سحاب
    int off = (int)(rcBg * 0.35f);
    for (int i = -1; i <= SW / 9 + 1; i++) { int idx = i + off / 9 + 1000, bx = i * 9 - off % 9, bh = 10 + hh(idx * 7) % 30;
      cv.fillRect(bx, RC_HOR - bh, 8, bh, mix(RGB(88, 98, 130), rcFog, 120));
      for (int wy = RC_HOR - bh + 2; wy < RC_HOR - 2; wy += 4) { if (hh(idx * 13 + wy) & 1) cv.drawPixel(bx + 2, wy, RGB(235, 220, 140)); if (hh(idx * 17 + wy) & 1) cv.drawPixel(bx + 5, wy, RGB(235, 220, 140)); } }
    int off2 = (int)(rcBg * 0.6f);
    for (int i = -1; i <= SW / 14 + 1; i++) { int idx = i + off2 / 14 + 2000, bx = i * 14 - off2 % 14, bh = 6 + hh(idx * 5) % 18;
      cv.fillRect(bx, RC_HOR - bh, 13, bh, RGB(62, 70, 98));
      for (int wy = RC_HOR - bh + 2; wy < RC_HOR - 1; wy += 3) for (int wx = bx + 2; wx < bx + 12; wx += 3) if (hh(idx * 3 + wy * 5 + wx) & 1) cv.drawPixel(wx, wy, RGB(250, 225, 120)); }
  } else {   // ليل نيون (الأصلي)
    for (int k = 0; k < 14; k++) cv.drawPixel(hh(k * 7) % SW, 13 + hh(k * 13) % 22, RGB(200, 200, 255));
    int sunx = 64 - (int)(rcBg * 0.3f) % 200; if (sunx < -40) sunx += 240;
    cv.fillCircle(sunx, RC_HOR - 8, 18, RGB(255, 190, 70));
    for (int y = RC_HOR - 14; y < RC_HOR; y += 3) cv.fillRect(sunx - 18, y, 37, (y - (RC_HOR - 14)) / 5 + 1, mix(RGB(12, 6, 48), rcFog, (y - 12) * 255 / (RC_HOR - 12)));
    for (int x = 0; x < SW; x++) {
      float u = x + rcBg; int mh = 8 + (int)(9 * sinf(u * 0.05f) + 5 * sinf(u * 0.13f + 1.0f)); if (mh < 2) mh = 2;
      cv.drawFastVLine(x, RC_HOR - mh, mh, RGB(34, 8, 76)); cv.drawPixel(x, RC_HOR - mh, RGB(255, 60, 200));
    }
  }
  float dxA = 0, xo = 0;
  for (int i = RC_H; i >= 1; i--) {
    float zr = RC_CAM / i, dist = rcPz + zr;
    dxA += rcCurve(dist) * 0.011f; xo += dxA;
    float hw = (float)i / RC_H * 74.0f, cx = 64 + xo - rcPx * hw;
    rcCx[i] = cx; rcHw[i] = hw;
    int y = RC_HOR + i; if (y >= SH) continue;
    int band = ((int)(dist / 5.0f)) & 1;
    uint8_t fogA = (uint8_t)((RC_H - i) * 217 / RC_H);
    uint16_t grass = mix(band ? ZGRA[zn] : ZGRB[zn], rcFog, fogA);
    uint16_t road = mix(band ? ZROA[zn] : ZROB[zn], rcFog, fogA);
    uint16_t rum = mix(band ? ZRUA[zn] : ZRUB[zn], rcFog, fogA);
    int xl = (int)(cx - hw), xr = (int)(cx + hw), rw = (int)(hw * 0.12f) + 1;
    hl(0, SW - 1, y, grass); hl(xl - rw, xl - 1, y, rum); hl(xr + 1, xr + rw, y, rum); hl(xl, xr, y, road);
    uint16_t edge = mix(zn == 2 ? RGB(0, 255, 255) : RGB(250, 250, 250), rcFog, fogA); px(xl, y, edge); px(xr, y, edge);
    if (((int)(dist / 4.0f)) & 1) {
      int lw = (int)(hw * 0.025f) + 1; uint16_t lc = mix(C_W, rcFog, fogA);
      int x1 = (int)(cx - hw / 3), x2 = (int)(cx + hw / 3); hl(x1, x1 + lw, y, lc); hl(x2, x2 + lw, y, lc);
    }
  }
  for (int pass = 0; pass < 2; pass++)
  for (int k = 22; k >= 1; k--) {
    float zj = (floorf(rcPz / 14.0f) + k) * 14.0f, rel = zj - rcPz;
    if (rel < 5 || rel >= RC_CAM) continue;
    int i = (int)(RC_CAM / rel); if (i < 2 || i >= RC_H) continue;
    uint32_t id = (uint32_t)(zj / 14.0f) + pass * 977u; int side = (hh(id) & 1) ? 1 : -1; uint32_t hv = hh(id * 3 + 1);
    int sy = RC_HOR + i, ph = i * 34 / RC_H + 2; uint8_t fa = (uint8_t)((RC_H - i) * 200 / RC_H);
    if (zn == 0) {
      if (pass == 1) {   // حقول بعيدة فيها أبقار وأشجار
        if (hv % 3) continue; int sx = (int)(rcCx[i] + side * rcHw[i] * (2.3f + ((hv >> 4) & 7) * 0.45f));
        if (hv & 8) rcCow(sx, sy, ph, (hv & 16) != 0, fa); else rcTree(sx, sy, ph, fa);
        continue;
      }
      int kind = hv & 7, sx = (int)(rcCx[i] + side * rcHw[i] * 1.55f);
      if (kind <= 2 || kind == 7) rcTree(sx, sy, ph, fa);
      else if (kind == 3) { cv.drawFastVLine(sx, sy - ph, ph, mix(RGB(150, 110, 60), rcFog, fa)); cv.drawFastHLine(sx - ph / 3, sy - ph * 2 / 3, ph * 2 / 3 + 1, mix(RGB(170, 130, 80), rcFog, fa)); }
      else if (kind == 6) cv.fillCircle(sx, sy - ph / 4, ph / 3 + 1, mix(RGB(50, 140, 60), rcFog, fa));
      else rcCow((int)(rcCx[i] + side * rcHw[i] * (1.9f + ((hv >> 4) & 3) * 0.3f)), sy, ph, side > 0, fa);
    } else if (zn == 1) {
      if (pass == 1) continue;
      int kind = hv & 7;
      if (kind == 4) { int sx = (int)(rcCx[i] + side * rcHw[i] * 1.5f); cv.drawFastVLine(sx, sy - ph, ph, mix(RGB(90, 90, 100), rcFog, fa)); cv.fillRect(sx - 1, sy - ph - 1, 3, 2, mix(RGB(255, 235, 150), rcFog, fa)); }
      else if (kind == 5) rcTree((int)(rcCx[i] + side * rcHw[i] * 1.5f), sy, ph, fa);
      else rcBuilding((int)(rcCx[i] + side * rcHw[i] * 1.95f), sy, ph, hv, fa);
    } else {
      if (pass == 1) continue;
      int kind = hv & 3, sx = (int)(rcCx[i] + side * rcHw[i] * 1.55f);
      switch (kind) {
        case 0: cv.drawFastVLine(sx, sy - ph, ph, mix(RGB(90, 90, 140), rcFog, fa)); cv.fillRect(sx - 1, sy - ph - 1, 3, 2, mix(RGB(0, 255, 255), rcFog, fa)); break;
        case 1: cv.fillTriangle(sx - ph / 3, sy, sx + ph / 3, sy, sx, sy - ph, mix(RGB(10, 50, 70), rcFog, fa)); cv.drawLine(sx - ph / 3, sy, sx, sy - ph, mix(RGB(0, 255, 200), rcFog, fa)); break;
        case 2: cv.fillTriangle(sx - ph / 4, sy - ph / 2, sx + ph / 4, sy - ph / 2, sx, sy - ph, mix(RGB(255, 60, 200), rcFog, fa)); cv.fillTriangle(sx - ph / 4, sy - ph / 2, sx + ph / 4, sy - ph / 2, sx, sy, mix(RGB(120, 20, 150), rcFog, fa)); break;
        default: cv.drawFastVLine(sx, sy - ph, ph, mix(RGB(90, 90, 140), rcFog, fa)); cv.fillRect(sx - ph / 3, sy - ph, ph * 2 / 3 + 1, ph / 3 + 1, mix(RGB(255, 220, 60), rcFog, fa)); break;
      }
    }
  }
  for (auto &c : rcCn) {
    if (!c.on) continue; float rel = c.z - rcPz; if (rel < 5.0f || rel >= RC_CAM) continue;
    int i = (int)(RC_CAM / rel); if (i < 2 || i >= RC_H) continue;
    int cx = (int)(rcCx[i] + c.lx * rcHw[i]), y = RC_HOR + i, r = (int)(rcHw[i] * 0.075f) + 2;
    float spn = fabsf(cosf(millis() * 0.008f + c.z)); int rw = (int)(r * (0.3f + 0.7f * spn)); if (rw < 1) rw = 1;
    uint8_t fa = (uint8_t)((RC_H - i) * 160 / RC_H);
    uint16_t gold = mix(RGB(255, 210, 40), rcFog, fa), edge = mix(RGB(255, 255, 200), rcFog, fa);
    cv.fillRoundRect(cx - rw, y - r * 2, rw * 2 + 1, r * 2, rw, gold); cv.drawRoundRect(cx - rw, y - r * 2, rw * 2 + 1, r * 2, rw, edge);
  }
  int ord[7]; for (int k = 0; k < 7; k++) ord[k] = k;
  for (int a = 1; a < 7; a++) { int v = ord[a], b = a - 1; while (b >= 0 && rcCars[ord[b]].z < rcCars[v].z) { ord[b + 1] = ord[b]; b--; } ord[b + 1] = v; }
  for (int k = 0; k < 7; k++) {
    RcCar &c = rcCars[ord[k]]; float rel = c.z - rcPz; if (rel < 4.9f || rel >= RC_CAM) continue;
    rcCarDraw((int)(RC_CAM / rel), c.lx, c.col);
  }
  if (rcSpd > 70) for (int k = 0; k < 6; k++) { int x = random(2) ? random(0, 14) : random(114, 128), y = random(30, 110); cv.drawFastVLine(x, y, 8 + random(8), RGB(200, 220, 255)); }
  if (!(rcInv > 0 && ((int)(rcInv * 14) & 1)))
    rcPlayerCar(64 + (int)(rcSteer * 3), 0, (int)(rcSteer * 2), CARS[rcSel], held[K_B], held[K_A] && rcNit > 0.02f);
  char b[24]; snprintf(b, 16, "%dKM/H", (int)(rcSpd * 3.2f)); txtS(b, 3, 14, C_W);
  cv.drawRect(72, 14, 54, 6, RGB(120, 130, 200)); cv.fillRect(74, 16, (int)(50 * rcNit), 2, RGB(120, 200, 255));
  snprintf(b, 24, "$ +%u", (unsigned)rcRunCoins); txtS(b, 3, 24, RGB(255, 220, 60));
  if (rcZoneT > 0) txtS(ZNAME[zn], (SW - (int)strlen(ZNAME[zn]) * 6) / 2, 44, C_W);
  drawHud();
}

// ---------- 9) NIGHTMARE (رعب: متاهة 3D من منظور الشخص الأول + وحش بيطاردك) ----------
#define HM 21
uint8_t hmap[HM][HM]; bool hseen[HM][HM]; int hdist[HM][HM];
struct HItem { int8_t x, y; uint8_t k; bool on; };
HItem hitm[6];
float hpx, hpy, hang, hStam, hBat, hBob, hWalk, hFear, hFlick, hFlickT, hMsgT, hScareT, hHaluT, hHaluShow, hHeartT, hCreakT;
bool hTorch, hCaught, hSprint, hMoving, hMHunt;
int hFuses, hNeed, hLvl;
float hmx, hmy, hmLost; int hmcx, hmcy, hmtx, hmty, hwgx, hwgy;
const char *hMsg = "";
float hzb[SW], hcone[SW]; uint8_t hconeI[SW], hrow[SH]; uint16_t hfloorLUT[256], hceilLUT[256]; bool hLutDone = false;

static inline uint16_t shade(uint8_t r, uint8_t g, uint8_t b, int br) { return RGB((r * br) >> 8, (g * br) >> 8, (b * br) >> 8); }
static inline int fall255(float d) { return (int)(255.0f / (1.0f + 0.32f * d * d)); }
bool hSolid(float x, float y) { int cx = (int)x, cy = (int)y; if (x < 0 || y < 0 || cx >= HM || cy >= HM) return true; return hmap[cy][cx]; }
bool hLos(float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0, d = sqrtf(dx * dx + dy * dy); int n = (int)(d / 0.12f) + 1;
  for (int k = 1; k < n; k++) if (hSolid(x0 + dx * k / n, y0 + dy * k / n)) return false;
  return true;
}
void hGen() {
  memset(hmap, 1, sizeof(hmap)); memset(hseen, 0, sizeof(hseen));
  int stk[100][2], sp = 0; bool vis[10][10]; memset(vis, 0, sizeof(vis));
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  vis[0][0] = true; hmap[1][1] = 0; stk[sp][0] = 0; stk[sp][1] = 0; sp++;
  while (sp) {
    int cx = stk[sp - 1][0], cy = stk[sp - 1][1], nb[4], n = 0;
    for (int d = 0; d < 4; d++) { int nx = cx + dx4[d], ny = cy + dy4[d]; if (nx >= 0 && ny >= 0 && nx < 10 && ny < 10 && !vis[ny][nx]) nb[n++] = d; }
    if (!n) { sp--; continue; }
    int d = nb[random(n)], nx = cx + dx4[d], ny = cy + dy4[d];
    hmap[2 * cy + 1 + dy4[d]][2 * cx + 1 + dx4[d]] = 0; hmap[2 * ny + 1][2 * nx + 1] = 0; vis[ny][nx] = true;
    stk[sp][0] = nx; stk[sp][1] = ny; sp++;
  }
  for (int k = 0; k < 26; k++) {
    int x = random(1, HM - 1), y = random(1, HM - 1);
    if (hmap[y][x] && ((!hmap[y][x - 1] && !hmap[y][x + 1]) || (!hmap[y - 1][x] && !hmap[y + 1][x]))) hmap[y][x] = 0;
  }
}
void hBfs(int gx, int gy) {
  static int q[HM * HM]; int qh = 0, qt = 0;
  for (int y = 0; y < HM; y++) for (int x = 0; x < HM; x++) hdist[y][x] = 999;
  hdist[gy][gx] = 0; q[qt++] = gy * HM + gx;
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  while (qh < qt) {
    int c = q[qh++], x = c % HM, y = c / HM;
    for (int d = 0; d < 4; d++) { int nx = x + dx4[d], ny = y + dy4[d]; if (nx >= 0 && ny >= 0 && nx < HM && ny < HM && !hmap[ny][nx] && hdist[ny][nx] > hdist[y][x] + 1) { hdist[ny][nx] = hdist[y][x] + 1; q[qt++] = ny * HM + nx; } }
  }
}
void hPickNext() {
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  int best = hdist[hmcy][hmcx], bx = hmcx, by = hmcy;
  for (int d = 0; d < 4; d++) { int nx = hmcx + dx4[d], ny = hmcy + dy4[d]; if (nx >= 0 && ny >= 0 && nx < HM && ny < HM && hdist[ny][nx] < best) { best = hdist[ny][nx]; bx = nx; by = ny; } }
  hmtx = bx; hmty = by;
}
void hPlace(int idx, uint8_t k, int minD) {
  for (int t = 0; t < 200; t++) {
    int x = 1 + 2 * random(10), y = 1 + 2 * random(10);
    if (abs(x - 1) + abs(y - 1) < minD) continue;
    if ((x == 19 && y == 19) || (x == 19 && y == 1)) continue;
    bool ok = true; for (int j = 0; j < idx; j++) if (hitm[j].on && hitm[j].x == x && hitm[j].y == y) ok = false;
    if (!ok) continue;
    hitm[idx].x = x; hitm[idx].y = y; hitm[idx].k = k; hitm[idx].on = true; return;
  }
  hitm[idx].on = false;
}
void hNewLevel() {
  hGen(); hpx = 1.5f; hpy = 1.5f; hang = hmap[1][2] == 0 ? 0 : PI / 2; hFuses = 0; hNeed = 3;
  for (int i = 0; i < 3; i++) hPlace(i, 0, 14); for (int i = 3; i < 6; i++) hPlace(i, 1, 4);
  hmx = 19.5f; hmy = 1.5f; hmcx = 19; hmcy = 1; hmtx = 19; hmty = 1; hMHunt = false; hmLost = 0; hwgx = 19; hwgy = 19;
  hCaught = false; hFear = 0; hMsgT = 3.0f; hMsg = "FIND 3 FUSES"; hHaluT = 15; hHaluShow = 0; hHeartT = 1; hCreakT = 10; hFlick = 1; hFlickT = 0;
}
void hInit0() {
  if (!hLutDone) {
    hLutDone = true;
    for (int b = 0; b < 256; b++) { hfloorLUT[b] = shade(70, 60, 52, b); hceilLUT[b] = shade(36, 34, 46, b); }
    for (int x = 0; x < SW; x++) { float u = fabsf(x - 63.5f) / 58.0f; hcone[x] = u >= 1 ? 0 : powf(1 - u, 1.1f); }
  }
  lives = 0; hLvl = 1; hBat = 1; hStam = 1; hTorch = true; hBob = 0; hWalk = 0; hScareT = 0; hNewLevel();
}
void hMonster(float dt) {
  float dx = hpx - hmx, dy = hpy - hmy, d = sqrtf(dx * dx + dy * dy);
  bool los = hLos(hmx, hmy, hpx, hpy);
  float vis = hTorch ? 9.0f : 3.2f, hear = hSprint ? 7.0f : (hMoving ? 2.6f : 1.2f);
  if ((los && d < vis) || d < hear) { if (!hMHunt) { hMHunt = true; SFX(SFX_GROWL); } hmLost = 0; }
  else if (hMHunt) { hmLost += dt; if (hmLost > 6) hMHunt = false; }
  float spd = hMHunt ? fminf(2.4f, 1.35f + 0.12f * hLvl + 0.14f * hFuses) : 0.85f;
  if (hMHunt && los && d < 2.0f) {
    float st = spd * dt; if (st > d) st = d; hmx += dx / d * st; hmy += dy / d * st; hmcx = (int)hmx; hmcy = (int)hmy; hmtx = hmcx; hmty = hmcy;
  } else {
    float tx = hmtx + 0.5f, ty = hmty + 0.5f, ddx = tx - hmx, ddy = ty - hmy, dd = sqrtf(ddx * ddx + ddy * ddy);
    if (dd < 0.06f) {
      hmcx = hmtx; hmcy = hmty;
      if (hMHunt) hBfs((int)hpx, (int)hpy);
      else { if ((hmcx == hwgx && hmcy == hwgy) || hmap[hwgy][hwgx]) { hwgx = 1 + 2 * random(10); hwgy = 1 + 2 * random(10); } hBfs(hwgx, hwgy); }
      hPickNext();
    } else { float st = spd * dt; if (st > dd) st = dd; hmx += ddx / dd * st; hmy += ddy / dd * st; }
  }
  if (d < 0.6f && !hCaught) { hCaught = true; hScareT = 1.5f; SFX(SFX_SCARE); shake = 0.6f; }
}
void hUpd0(float dt) {
  if (hCaught) { hScareT -= dt; if (hScareT <= 0) die(); return; }
  hang += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 2.3f * sensM() * dt;
  float mv = (held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0);
  hMoving = mv != 0; hSprint = held[K_A] && mv > 0 && hStam > 0.05f;
  float sp = hSprint ? 3.0f : 1.7f; if (mv < 0) sp *= 0.6f;
  if (hSprint) hStam -= dt * 0.28f; else hStam = fminf(1, hStam + dt * (hMoving ? 0.12f : 0.25f));
  if (hStam < 0) hStam = 0;
  float dx = cosf(hang) * mv * sp * dt, dy = sinf(hang) * mv * sp * dt, r = 0.2f, nx = hpx + dx;
  if (!hSolid(nx + r, hpy + r) && !hSolid(nx - r, hpy + r) && !hSolid(nx + r, hpy - r) && !hSolid(nx - r, hpy - r)) hpx = nx;
  float ny = hpy + dy;
  if (!hSolid(hpx + r, ny + r) && !hSolid(hpx - r, ny + r) && !hSolid(hpx + r, ny - r) && !hSolid(hpx - r, ny - r)) hpy = ny;
  if (hMoving) { hWalk += dt * (hSprint ? 12 : 7); hBob = sinf(hWalk) * (hSprint ? 2.2f : 1.2f); } else hBob *= 0.9f;
  if (prs[K_B] && (hBat > 0.01f || hTorch)) { hTorch = !hTorch; sndL(hTorch ? 1100 : 700, 24); }
  if (hTorch) { hBat -= dt / 95.0f; if (hBat <= 0) { hBat = 0; hTorch = false; hMsg = "TORCH DEAD"; hMsgT = 2; } }
  hFlickT -= dt;
  if (hFlickT <= 0) { hFlickT = 0.04f + random(120) / 1000.0f; float p = hFear * 0.35f + (hBat < 0.2f ? 0.3f : 0.02f); hFlick = (random(1000) / 1000.0f < p) ? 0.15f + random(300) / 1000.0f : 1.0f; }
  if (hMsgT > 0) hMsgT -= dt;
  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) { int cx = (int)hpx + x, cy = (int)hpy + y; if (cx >= 0 && cy >= 0 && cx < HM && cy < HM) hseen[cy][cx] = true; }
  for (auto &it : hitm) if (it.on) {
    float ddx = it.x + 0.5f - hpx, ddy = it.y + 0.5f - hpy;
    if (ddx * ddx + ddy * ddy < 0.3f) {
      it.on = false; SFX(SFX_POWER);
      if (it.k == 0) { hFuses++; score += 100; if (hFuses >= hNeed) { hMsg = "EXIT OPEN - RUN!"; hMsgT = 3; hMHunt = true; hmLost = -3; SFX(SFX_GROWL); } else { hMsg = "FUSE FOUND"; hMsgT = 1.5f; } }
      else { hBat = fminf(1, hBat + 0.5f); hMsg = "BATTERY"; hMsgT = 1.5f; }
    }
  }
  if (hFuses >= hNeed) {
    float ddx = 19.5f - hpx, ddy = 19.5f - hpy;
    if (ddx * ddx + ddy * ddy < 0.4f) { score += 500 * hLvl; hLvl++; SFX(SFX_WIN); hNewLevel(); hMsg = "NEXT LEVEL"; return; }
  }
  hMonster(dt);
  float dm = sqrtf((hpx - hmx) * (hpx - hmx) + (hpy - hmy) * (hpy - hmy));
  float f = 1.0f - dm / 9.0f; if (f < 0) f = 0;
  if (!hLos(hmx, hmy, hpx, hpy)) f *= 0.65f; if (hMHunt) f = fmaxf(f, 0.35f);
  hFear += (f - hFear) * fminf(1, dt * 3);
  hHeartT -= dt; if (hHeartT <= 0) { hHeartT = 60.0f / (55.0f + hFear * 110.0f); if (hFear > 0.12f) SFXL(SFX_HEART); }
  hCreakT -= dt; if (hCreakT <= 0) { hCreakT = 14 + random(160) / 10.0f; SFXL(SFX_CREAK); }
  hHaluT -= dt; if (hHaluT <= 0) { hHaluT = 18 + random(220) / 10.0f; hHaluShow = 0.35f; }
  if (hHaluShow > 0) hHaluShow -= dt;
}
struct Spr { float tx, ty; uint8_t id; };
uint8_t monShape(float u, float v, bool close) {
  float du = u - 0.5f, ad = fabsf(du);
  { float ex = ad - 0.055f, ey = v - 0.125f; if (ex * ex + ey * ey < 0.0009f) return 2; }
  if ((du * du) / 0.0150f + ((v - 0.13f) * (v - 0.13f)) / 0.0105f < 1.0f) { if (close && v > 0.17f && v < 0.21f && ad < 0.06f) return 3; return 1; }
  if (v >= 0.2f && v < 0.28f && ad < 0.045f) return 1;
  if (v >= 0.27f && v < 0.62f) { float w = 0.17f - (v - 0.27f) * 0.2f; if (ad < w) return 1; }
  if (v >= 0.29f && v < 0.9f) { float c = 0.17f + (v - 0.29f) * 0.25f, th = v > 0.78f ? 0.05f : 0.028f; if (fabsf(ad - c) < th) return 1; }
  if (v >= 0.62f && v <= 1.0f) { float lc = 0.07f + (v - 0.62f) * 0.04f; if (fabsf(ad - lc) < 0.045f) return 1; }
  return 0;
}
void hSprites() {
  float dirX = cosf(hang), dirY = sinf(hang), plX = -dirY * 0.66f, plY = dirX * 0.66f, inv = 1.0f / (plX * dirY - dirX * plY);
  int hz = 64 + (int)hBob;
  Spr L[9]; int n = 0;
  auto add = [&](float wx, float wy, uint8_t id) {
    float sx = wx - hpx, sy = wy - hpy, tX = inv * (dirY * sx - dirX * sy), tY = inv * (-plY * sx + plX * sy);
    if (tY > 0.2f && n < 9) { L[n].tx = tX; L[n].ty = tY; L[n].id = id; n++; }
  };
  for (int i = 0; i < 6; i++) if (hitm[i].on) add(hitm[i].x + 0.5f, hitm[i].y + 0.5f, i);
  add(19.5f, 19.5f, 6);
  add(hmx, hmy, 7);
  if (hHaluShow > 0) add(hpx + dirX * 5, hpy + dirY * 5, 8);
  for (int a = 1; a < n; a++) { Spr v = L[a]; int b = a - 1; while (b >= 0 && L[b].ty < v.ty) { L[b + 1] = L[b]; b--; } L[b + 1] = v; }
  float pulse = 0.5f + 0.5f * sinf(millis() * 0.006f);
  for (int k = 0; k < n; k++) {
    Spr &s = L[k]; int scx = (int)(SW / 2 * (1 + s.tx / s.ty)), H = (int)(SH / s.ty);
    int fl = fall255(s.ty);
    if (s.id < 6) {
      int size = H * 28 / 100; if (size < 2) size = 2;
      int cy = hz + H / 2 - size / 2 - H * 12 / 100 - (int)(sinf(millis() * 0.004f + s.id) * H * 0.03f);
      uint8_t r = 255, g = 230, b = 60; if (hitm[s.id].k == 1) { r = 80; g = 255; b = 120; }
      for (int xx = scx - size / 2; xx <= scx + size / 2; xx++) {
        if (xx < 0 || xx >= SW || s.ty >= hzb[xx]) continue;
        int br = 140 + ((hconeI[xx] * fl) >> 8) / 2; if (br > 255) br = 255;
        for (int yy = cy - size / 2; yy <= cy + size / 2; yy++) if (yy >= 0 && yy < SH) FB[yy * SW + xx] = (xx == scx) ? C_W : shade(r, g, b, br);
      }
    } else if (s.id == 6) {
      int w = H / 2, h = H * 9 / 10; bool open = hFuses >= hNeed;
      uint8_t r = open ? 40 : 120, g = open ? (uint8_t)(160 + pulse * 90) : 20, b = open ? 80 : 20;
      for (int xx = scx - w / 2; xx <= scx + w / 2; xx++) {
        if (xx < 0 || xx >= SW || s.ty >= hzb[xx]) continue;
        int br = 120 + ((hconeI[xx] * fl) >> 8) / 2; if (br > 255) br = 255;
        for (int yy = hz - h / 2; yy <= hz + h / 2; yy++) if (yy >= 0 && yy < SH) FB[yy * SW + xx] = (xx == scx - w / 2 || xx == scx + w / 2 || yy == hz - h / 2) ? C_W : shade(r, g, b, br);
      }
    } else {
      bool ph = s.id == 8; int wM = H * 55 / 100, MH = H * 105 / 100; if (wM < 2) continue;
      int sx0 = scx - wM / 2, sy0 = hz - MH / 2; bool close = s.ty < 3.5f; int gl = random(100) < 5 ? random(-3, 4) : 0;
      for (int xx = sx0; xx < sx0 + wM; xx++) {
        int dx2 = xx + gl; if (dx2 < 0 || dx2 >= SW || s.ty >= hzb[dx2]) continue;
        float u = (float)(xx - sx0) / wM + sinf(millis() * 0.003f) * 0.02f;
        int bm = 20 + ((hconeI[dx2] * fl) >> 8); if (bm > 255) bm = 255;
        for (int yy = (sy0 < 0 ? 0 : sy0); yy < sy0 + MH && yy < SH; yy++) {
          uint8_t kd = monShape(u, (float)(yy - sy0) / MH, close); if (!kd) continue;
          if (ph && ((dx2 + yy) & 1)) continue;
          FB[yy * SW + dx2] = kd == 1 ? shade(46, 22, 28, bm) : (kd == 2 ? RGB(255, ph ? 90 : 30, 30) : shade(225, 220, 205, bm < 90 ? 90 : bm));
        }
      }
    }
  }
}
void hDrawView() {
  float dirX = cosf(hang), dirY = sinf(hang), plX = -dirY * 0.66f, plY = dirX * 0.66f;
  int hz = 64 + (int)hBob;
  float torchP = hTorch ? hFlick * (0.3f + 0.7f * fminf(1.0f, hBat * 3.0f)) : 0.0f;
  for (int x = 0; x < SW; x++) hconeI[x] = (uint8_t)(255.0f * torchP * hcone[x]);
  for (int y = 0; y < SH; y++) { float dd = y > hz ? 64.0f / (y - hz) : (y < hz ? 64.0f / (hz - y) : 99.0f); hrow[y] = (uint8_t)fall255(dd); }
  for (int x = 0; x < SW; x++) {
    float camX = 2.0f * x / SW - 1.0f, rdx = dirX + plX * camX, rdy = dirY + plY * camX;
    int mX = (int)hpx, mY = (int)hpy, stX, stY, side = 0, guard = 0;
    float ddx = rdx == 0 ? 1e30f : fabsf(1.0f / rdx), ddy = rdy == 0 ? 1e30f : fabsf(1.0f / rdy), sdX, sdY;
    if (rdx < 0) { stX = -1; sdX = (hpx - mX) * ddx; } else { stX = 1; sdX = (mX + 1.0f - hpx) * ddx; }
    if (rdy < 0) { stY = -1; sdY = (hpy - mY) * ddy; } else { stY = 1; sdY = (mY + 1.0f - hpy) * ddy; }
    while (guard++ < 40) {
      if (sdX < sdY) { sdX += ddx; mX += stX; side = 0; } else { sdY += ddy; mY += stY; side = 1; }
      if (mX < 0 || mY < 0 || mX >= HM || mY >= HM) break;
      if (hmap[mY][mX]) break;
    }
    float perp = side == 0 ? sdX - ddx : sdY - ddy; if (perp < 0.05f) perp = 0.05f;
    hzb[x] = perp;
    float wallX = side == 0 ? hpy + perp * rdy : hpx + perp * rdx; wallX -= floorf(wallX);
    int lineH = (int)(SH / perp); if (lineH < 1) lineH = 1;
    int y0 = hz - lineH / 2, y1 = hz + lineH / 2, ys = y0 < 0 ? 0 : y0, ye = y1 >= SH ? SH - 1 : y1;
    int br = 14 + ((hconeI[x] * fall255(perp)) >> 8); br = (br * (side ? 200 : 255)) >> 8; if (br > 255) br = 255;
    bool bloody = (hh(mX * 31 + mY * 17) % 9) == 0;
    uint16_t cMor = shade(70, 64, 60, br), cA, cB;
    if (bloody) { cA = shade(120, 12, 12, br); cB = shade(88, 8, 8, br); } else { cA = shade(110, 52, 40, br); cB = shade(88, 42, 34, br); }
    for (int y = 0; y < ys; y++) { int b = 10 + ((hconeI[x] * hrow[y]) >> 8); if (b > 255) b = 255; FB[y * SW + x] = hceilLUT[b]; }
    int tx = (int)(wallX * 16) & 15; if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) tx = 15 - tx;
    int step = (16 << 16) / lineH, pos = (ys - y0) * step;
    for (int y = ys; y <= ye; y++) {
      int ty = (pos >> 16) & 15; pos += step; int row = ty >> 2; uint16_t c;
      if ((ty & 3) == 0) c = cMor; else { int bx = tx + (row & 1) * 4; if ((bx & 7) == 0) c = cMor; else c = (hh(row * 3 + (bx >> 3) + mX * 5 + mY * 11) & 1) ? cA : cB; }
      FB[y * SW + x] = c;
    }
    for (int y = ye + 1; y < SH; y++) { int b = 10 + ((hconeI[x] * hrow[y]) >> 8); if (b > 255) b = 255; FB[y * SW + x] = hfloorLUT[b]; }
  }
}
void hScareFace(float k) {
  cv.fillScreen(C_K);
  for (int i = 0; i < 400; i++) { uint8_t g = random(20, 160); cv.drawPixel(random(SW), random(SH), RGB(g, g, g)); }
  float g = fminf(1.0f, k * 4.0f); int R = (int)(30 + 52 * g), cx = 64 + random(-3, 4), cy = 62 + random(-3, 4);
  cv.fillCircle(cx, cy, R, RGB(48, 42, 38)); cv.drawCircle(cx, cy, R, RGB(90, 20, 20));
  int ex = (int)(R * 0.38f), ey = (int)(R * 0.25f), er = (int)(R * 0.22f);
  cv.fillCircle(cx - ex, cy - ey, er, C_K); cv.fillCircle(cx + ex, cy - ey, er, C_K);
  cv.fillCircle(cx - ex, cy - ey, er / 4 + 1, RGB(255, 20, 20)); cv.fillCircle(cx + ex, cy - ey, er / 4 + 1, RGB(255, 20, 20));
  cv.fillTriangle(cx - R / 10, cy + R / 8, cx + R / 10, cy + R / 8, cx, cy - R / 25, C_K);
  int mw = (int)(R * 1.1f), mh = (int)(R * 0.4f), my = cy + (int)(R * 0.35f);
  cv.fillRect(cx - mw / 2, my, mw, mh, RGB(30, 0, 0));
  for (int i = 0; i < 8; i++) { int x0 = cx - mw / 2 + i * mw / 8; cv.fillTriangle(x0, my, x0 + mw / 8, my, x0 + mw / 16, my + mh / 2, RGB(230, 225, 205)); cv.fillTriangle(x0, my + mh, x0 + mw / 8, my + mh, x0 + mw / 16, my + mh / 2, RGB(230, 225, 205)); }
  for (int i = 0; i < 4; i++) cv.drawLine(cx + random(-R, R), cy - R, cx + random(-R, R), cy + random(0, R), RGB(20, 10, 10));
  for (int y = random(0, 20); y < SH; y += random(18, 40)) { int off = random(-10, 11); for (int yy = y; yy < y + 3 && yy < SH; yy++) for (int x = 0; x < SW; x++) { int sx = x + off; if (sx >= 0 && sx < SW) FB[yy * SW + x] = FB[yy * SW + sx]; } }
  if ((millis() / 80) & 1) for (int y = 0; y < SH; y += 2) hl(0, SW - 1, y, mix(FB[y * SW], RGB(255, 0, 0), 90));
}
static inline uint16_t redshift(uint16_t c) { int r = c >> 11, g = (c >> 5) & 63, b = c & 31; r += 5; if (r > 31) r = 31; return (r << 11) | ((g >> 1) << 5) | (b >> 1); }
void hDraw0() {
  if (hCaught) { hScareFace(1.0f - fmaxf(0, hScareT) / 1.5f); return; }
  hDrawView(); hSprites();
  int n = 30 + (int)(hFear * 260);
  for (int i = 0; i < n; i++) { uint8_t g = random(30, 200); FB[random(SW * SH)] = RGB(g, g, g); }
  if (hFear > 0.25f) {
    int th = (int)(hFear * 10);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
      int dmin = x; if (SW - 1 - x < dmin) dmin = SW - 1 - x; if (y < dmin) dmin = y; if (SH - 1 - y < dmin) dmin = SH - 1 - y;
      if (dmin >= th) { x = (x < th) ? SW - 1 - th : x; if (y >= th && y < SH - th && x < SW - th) x = SW - th - 1; continue; }
      FB[y * SW + x] = redshift(FB[y * SW + x]);
    }
  }
  cv.drawRect(3, 119, 20, 6, RGB(150, 150, 150)); cv.fillRect(23, 121, 2, 2, RGB(150, 150, 150));
  cv.fillRect(4, 120, (int)(18 * hBat), 4, hBat < 0.25f ? RGB(255, 40, 40) : RGB(230, 230, 120));
  cv.drawRect(28, 120, 32, 4, RGB(90, 120, 90)); cv.fillRect(29, 121, (int)(30 * hStam), 2, RGB(90, 220, 110));
  char b[16]; snprintf(b, 16, "FUSE %d/%d", hFuses, hNeed); txt(b, SW - 54, 119, RGB(255, 220, 80));
  if (hMsgT > 0 && hMsg[0]) txtS(hMsg, (SW - (int)strlen(hMsg) * 6) / 2, 6, RGB(255, 200, 200));
}
// ---- 9b) Nightmare: قصة افتتاحية + حفظ المستوى ----
int hPh = 0, hSavedLvl = 1;
bool nmLoad(NmSave &s) { return pref.getBytes("nsv", &s, sizeof(s)) == sizeof(s) && s.magic == 0x4E4D0001; }
void hSaveNow() { if (hPh != 2) return; NmSave s = {0x4E4D0001, hLvl, score}; pref.putBytes("nsv", &s, sizeof(s)); }
void hInit() {
  hInit0(); hSavedLvl = 1; NmSave s;
  if (nmLoad(s) && s.lvl > 1) hPh = 0; else { hPh = 1; storyStart(0); }
}
void hUpd(float dt) {
  if (hPh == 0) {
    noPause = true; NmSave s;
    if (prs[K_A] || prs[K_ST]) { if (nmLoad(s)) { hLvl = s.lvl; hNewLevel(); score = s.score; hSavedLvl = hLvl; } hPh = 2; SFX(SFX_OK); }
    else if (prs[K_B]) { pref.remove("nsv"); hPh = 1; storyStart(0); }
    return;
  }
  if (hPh == 1) { if (storyUpd(dt)) { hPh = 2; hSavedLvl = hLvl; } return; }
  hUpd0(dt);
  if (hLvl != hSavedLvl) { hSavedLvl = hLvl; hSaveNow(); }
}
void hDraw() {
  if (hPh == 0) { NmSave s; char b[24] = ""; if (nmLoad(s)) snprintf(b, 24, "LEVEL %d  SC %d", s.lvl, s.score); svMenuDraw("NIGHTMARE", b); return; }
  if (hPh == 1) { storyDraw(); return; }
  hDraw0();
}

// ---------- 10) ASTEROIDS (دوران + دفع + إطلاق + انقسام الصخور) ----------
struct Rock { float x, y, vx, vy, a, va, rad[9]; uint8_t sz; bool on; };
Rock rk[20];
struct Sb { float x, y, vx, vy, life; bool on; };
Sb sbul[6];
float asx, asy, asvx, asvy, asa, asCd, asInv, asWaveT; int asWave; bool asThr;
const float AR[3] = {4.5f, 8.0f, 13.0f};
void asSpawnRock(float x, float y, uint8_t sz, float sp) {
  for (auto &r : rk) if (!r.on) {
    r.on = true; r.x = x; r.y = y; r.sz = sz; float a = random(628) / 100.0f, s = sp * (0.6f + random(80) / 100.0f);
    r.vx = cosf(a) * s; r.vy = sinf(a) * s; r.a = 0; r.va = (random(200) - 100) / 100.0f * 1.2f;
    for (int k = 0; k < 9; k++) r.rad[k] = 0.7f + random(60) / 100.0f; break;
  }
}
void asWaveStart() {
  int n = 3 + asWave; if (n > 8) n = 8;
  for (int k = 0; k < n; k++) { float x, y; do { x = random(SW); y = 12 + random(SH - 12); } while (fabsf(x - asx) < 34 && fabsf(y - asy) < 34); asSpawnRock(x, y, 2, 16 + asWave * 2); }
}
void asInit() {
  lives = 3; asx = 64; asy = 70; asvx = asvy = 0; asa = -PI / 2; asCd = 0; asInv = 2; asWave = 1; asWaveT = 0;
  for (auto &r : rk) r.on = false; for (auto &b : sbul) b.on = false; asWaveStart();
}
void asUpd(float dt) {
  asa += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 4.2f * sensM() * 0.65f * dt;
  asThr = held[K_A] || held[K_UP];
  if (asThr) {
    asvx += cosf(asa) * 95 * dt; asvy += sinf(asa) * 95 * dt;
    float sp = sqrtf(asvx * asvx + asvy * asvy); if (sp > 115) { asvx *= 115 / sp; asvy *= 115 / sp; }
    if (random(3) == 0) burst(asx - cosf(asa) * 6, asy - sinf(asa) * 6, RGB(255, 160, 40), 1, 25, 0.35f);
  }
  asvx *= 1 - 0.35f * dt; asvy *= 1 - 0.35f * dt;
  asx += asvx * dt; asy += asvy * dt;
  if (asx < 0) asx += SW; if (asx >= SW) asx -= SW; if (asy < 12) asy += SH - 12; if (asy >= SH) asy -= SH - 12;
  asCd -= dt; asInv -= dt;
  if (held[K_B] && asCd <= 0) {
    asCd = 0.22f;
    for (auto &b : sbul) if (!b.on) { b.on = true; b.x = asx + cosf(asa) * 7; b.y = asy + sinf(asa) * 7; b.vx = cosf(asa) * 150 + asvx; b.vy = sinf(asa) * 150 + asvy; b.life = 1.1f; break; }
    SFXL(SFX_LASER);
  }
  for (auto &b : sbul) if (b.on) {
    b.x += b.vx * dt; b.y += b.vy * dt; b.life -= dt; if (b.life <= 0) b.on = false;
    if (b.x < 0) b.x += SW; if (b.x >= SW) b.x -= SW; if (b.y < 12) b.y += SH - 12; if (b.y >= SH) b.y -= SH - 12;
  }
  bool any = false;
  for (auto &r : rk) if (r.on) {
    any = true; r.x += r.vx * dt; r.y += r.vy * dt; r.a += r.va * dt;
    if (r.x < 0) r.x += SW; if (r.x >= SW) r.x -= SW; if (r.y < 12) r.y += SH - 12; if (r.y >= SH) r.y -= SH - 12;
    for (auto &b : sbul) if (b.on) {
      float dx = b.x - r.x, dy = b.y - r.y;
      if (dx * dx + dy * dy < AR[r.sz] * AR[r.sz] * 0.8f) {
        b.on = false; r.on = false; score += r.sz == 2 ? 20 : (r.sz == 1 ? 50 : 100);
        burst(r.x, r.y, r.sz == 2 ? RGB(0, 220, 255) : (r.sz == 1 ? RGB(80, 255, 140) : RGB(255, 230, 80)), 8 + r.sz * 4, 60); SFX(SFX_HIT);
        if (r.sz > 0) { float ox = r.x, oy = r.y; uint8_t ns = r.sz - 1; asSpawnRock(ox, oy, ns, 30 + asWave * 2); asSpawnRock(ox, oy, ns, 30 + asWave * 2); }
        break;
      }
    }
    if (r.on && asInv <= 0) {
      float dx = asx - r.x, dy = asy - r.y, rr = AR[r.sz] * 0.85f + 3;
      if (dx * dx + dy * dy < rr * rr) {
        lives--; shake = 0.25f; burst(asx, asy, RGB(120, 200, 255), 24, 90); SFX(SFX_BOOM); asInv = 2; asx = 64; asy = 70; asvx = asvy = 0;
        if (lives <= 0) { die(); return; }
      }
    }
  }
  if (!any) { asWaveT += dt; if (asWaveT > 1.4f) { asWave++; asWaveT = 0; asWaveStart(); SFX(SFX_POWER); } } else asWaveT = 0;
}
void asDraw() {
  vgrad(12, SH, RGB(2, 3, 14), RGB(10, 6, 28)); starsDraw(0.15f);
  for (auto &r : rk) if (r.on) {
    uint16_t c = r.sz == 2 ? RGB(0, 220, 255) : (r.sz == 1 ? RGB(80, 255, 140) : RGB(255, 230, 80));
    int lx = 0, ly = 0, fx = 0, fy = 0;
    for (int k = 0; k < 9; k++) {
      float a = r.a + k * 2 * PI / 9, rr = AR[r.sz] * r.rad[k]; int x = (int)(r.x + cosf(a) * rr), y = (int)(r.y + sinf(a) * rr);
      if (k) cv.drawLine(lx, ly, x, y, c); else { fx = x; fy = y; } lx = x; ly = y;
    }
    cv.drawLine(lx, ly, fx, fy, c);
  }
  for (auto &b : sbul) if (b.on) cv.fillRect((int)b.x - 1, (int)b.y - 1, 2, 2, C_W);
  if (!(asInv > 0 && ((int)(asInv * 12) & 1))) {
    float c = cosf(asa), s = sinf(asa);
    int nx = (int)(asx + c * 7), ny = (int)(asy + s * 7);
    int lx = (int)(asx + cosf(asa + 2.5f) * 6), ly = (int)(asy + sinf(asa + 2.5f) * 6), rx = (int)(asx + cosf(asa - 2.5f) * 6), ry = (int)(asy + sinf(asa - 2.5f) * 6);
    cv.drawLine(nx, ny, lx, ly, RGB(120, 220, 255)); cv.drawLine(nx, ny, rx, ry, RGB(120, 220, 255)); cv.drawLine(lx, ly, rx, ry, RGB(60, 140, 200));
    if (asThr) cv.drawLine((int)(asx - c * 4), (int)(asy - s * 4), (int)(asx - c * (8 + random(4))), (int)(asy - s * (8 + random(4))), RGB(255, 150, 30));
  }
  char b[8]; snprintf(b, 8, "W%d", asWave); txt(b, 3, 14, RGB(180, 180, 255));
}

// ======================= محرك 3D مشترك (Raycaster) + أدوات رعب =======================
static inline uint16_t shc(int r, int g, int b, int br) {
  if (r < 0) r = 0; if (r > 255) r = 255; if (g < 0) g = 0; if (g > 255) g = 255; if (b < 0) b = 0; if (b > 255) b = 255;
  if (br < 0) br = 0; if (br > 255) br = 255;
  return RGB((r * br) >> 8, (g * br) >> 8, (b * br) >> 8);
}
static inline int fallK(float d, float k) { return (int)(255.0f / (1.0f + k * d * d)); }
float zbc[SW]; uint8_t fcone[SW];
void setCone(float torch) { for (int x = 0; x < SW; x++) fcone[x] = (uint8_t)(255.0f * torch * hcone[x]); }
void coneInit() { for (int x = 0; x < SW; x++) { float u = fabsf(x - 63.5f) / 58.0f; hcone[x] = u >= 1 ? 0 : powf(1 - u, 1.1f); } }
static inline bool inEll(float x, float y, float cx, float cy, float rx, float ry) { float a = (x - cx) / rx, b = (y - cy) / ry; return a * a + b * b < 1.0f; }

void jobTask(void *) { for (;;) { xSemaphoreTake(semJ, portMAX_DELAY); jobFn(jobA, jobB); xSemaphoreGive(semJD); } }
void parRun(JobFn fn, int lo, int mid, int hi) {   // نص الشغل على Core 0 ونص على Core 1
  jobFn = fn; jobA = mid; jobB = hi; xSemaphoreGive(semJ); fn(lo, mid); xSemaphoreTake(semJD, portMAX_DELAY);
}
static const FpsV *gFv; static uint8_t gRow[SH]; static bool gSkipCeil = false;
void fpsCols(int xa, int xb) {
  const FpsV &v = *gFv; float dirX = cosf(v.a), dirY = sinf(v.a), plX = -dirY * 0.66f, plY = dirX * 0.66f; int hz = v.hz;
  for (int x = xa; x < xb; x++) {
    float camX = 2.0f * x / SW - 1.0f, rdx = dirX + plX * camX, rdy = dirY + plY * camX;
    int mX = (int)v.x, mY = (int)v.y, stX, stY, side = 0, guard = 0;
    float ddx = rdx == 0 ? 1e30f : fabsf(1.0f / rdx), ddy = rdy == 0 ? 1e30f : fabsf(1.0f / rdy), sdX, sdY;
    if (rdx < 0) { stX = -1; sdX = (v.x - mX) * ddx; } else { stX = 1; sdX = (mX + 1.0f - v.x) * ddx; }
    if (rdy < 0) { stY = -1; sdY = (v.y - mY) * ddy; } else { stY = 1; sdY = (mY + 1.0f - v.y) * ddy; }
    while (guard++ < 48) {
      if (sdX < sdY) { sdX += ddx; mX += stX; side = 0; } else { sdY += ddy; mY += stY; side = 1; }
      if (mX < 0 || mY < 0 || mX >= v.w || mY >= v.h) break;
      if (v.m[mY * v.w + mX]) break;
    }
    float perp = side == 0 ? sdX - ddx : sdY - ddy; if (perp < 0.05f) perp = 0.05f;
    zbc[x] = perp;
    float wallX = side == 0 ? v.y + perp * rdy : v.x + perp * rdx; wallX -= floorf(wallX);
    if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0)) wallX = 1.0f - wallX;
    int mc = constrain(mX, 0, v.w - 1), mr = constrain(mY, 0, v.h - 1);
    int H0 = (int)(SH / perp), lineH = (int)(SH * v.wh / perp); if (lineH < 1) lineH = 1;
    int y1 = hz + H0 / 2, y0 = y1 - lineH, ys = y0 < 0 ? 0 : y0, ye = y1 >= SH ? SH - 1 : y1;
    int br = v.amb + ((fcone[x] * fallK(perp, v.fk)) >> 8); br = (br * (side ? 200 : 255)) >> 8; if (br > 255) br = 255;
    int fa = v.fogK ? (int)fminf(255.0f, perp * v.fogK) : 0;
    if (!gSkipCeil) for (int y = 0; y < ys; y++) { int b = v.famb + (v.ccone ? ((fcone[x] * gRow[y]) >> 8) : 0); if (b > 255) b = 255; FB[y * SW + x] = v.cl[b]; }
    for (int y = ys; y <= ye; y++) {
      float vf = (float)(y - y0) / lineH;
      uint16_t c = v.wf(mc, mr, side, wallX, vf, br);
      if (fa) c = mix(c, v.fog, fa);
      FB[y * SW + x] = c;
    }
    for (int y = ye + 1; y < SH; y++) { int b = v.famb + ((fcone[x] * gRow[y]) >> 8); if (b > 255) b = 255; FB[y * SW + x] = v.fl[b]; }
  }
}
void fpsRender(const FpsV &v) {
  gFv = &v; int hz = v.hz;
  for (int y = 0; y < SH; y++) { float dd = y > hz ? 64.0f / (y - hz) : (y < hz ? 64.0f / (hz - y) : 99.0f); gRow[y] = (uint8_t)fallK(dd, v.fk); }
  parRun(fpsCols, 0, SW / 2, SW);
}
bool prj(float px0, float py0, float ang, float wx, float wy, int &scx, float &ty) {
  float dirX = cosf(ang), dirY = sinf(ang), plX = -dirY * 0.66f, plY = dirX * 0.66f, inv = 1.0f / (plX * dirY - dirX * plY);
  float sx = wx - px0, sy = wy - py0, tX = inv * (dirY * sx - dirX * sy); ty = inv * (-plY * sx + plX * sy);
  if (ty < 0.15f) return false;
  scx = (int)(SW / 2 * (1 + tX / ty)); return true;
}
// رسم Sprite مجسم (شكل بدالة) مع اختبار العمق
void drawSpr(const FpsV &vw, float wx, float wy, float hs, float ws, ShapeFn sf, ColFn cf) {
  int scx; float ty; if (!prj(vw.x, vw.y, vw.a, wx, wy, scx, ty)) return;
  int H = (int)(SH / ty), sh = (int)(H * hs), sw = (int)(H * ws); if (sw < 2 || sh < 2) return;
  int fl = fallK(ty, vw.fk), yb = vw.hz + H / 2, y0 = yb - sh, x0 = scx - sw / 2;
  int fa = vw.fogK ? (int)fminf(255.0f, ty * vw.fogK) : 0;
  for (int xx = x0; xx < x0 + sw; xx++) {
    if (xx < 0 || xx >= SW || ty >= zbc[xx]) continue;
    float u = (float)(xx - x0) / sw; int br = vw.amb + ((fcone[xx] * fl) >> 8); if (br > 255) br = 255;
    for (int yy = (y0 < 0 ? 0 : y0); yy < yb && yy < SH; yy++) {
      uint8_t k = sf(u, (float)(yy - y0) / sh); if (!k) continue;
      uint16_t c = cf(k, br); if (fa) c = mix(c, vw.fog, fa);
      FB[yy * SW + xx] = c;
    }
  }
}
bool solidG(const uint8_t *m, int w, int h, float x, float y) { int cx = (int)x, cy = (int)y; if (x < 0 || y < 0 || cx >= w || cy >= h) return true; return m[cy * w + cx] != 0; }
bool losG(const uint8_t *m, int w, int h, float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0, d = sqrtf(dx * dx + dy * dy); int n = (int)(d / 0.12f) + 1;
  for (int k = 1; k < n; k++) if (solidG(m, w, h, x0 + dx * k / n, y0 + dy * k / n)) return false;
  return true;
}
void bfsG(const uint8_t *m, int w, int h, int gx, int gy, int16_t *dist) {
  static uint16_t q[1600]; int qh = 0, qt = 0;
  for (int i = 0; i < w * h; i++) dist[i] = 999;
  if (gx < 0 || gy < 0 || gx >= w || gy >= h) return;
  dist[gy * w + gx] = 0; q[qt++] = gy * w + gx;
  static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
  while (qh < qt) {
    int c = q[qh++], x = c % w, y = c / w;
    for (int d = 0; d < 4; d++) { int nx = x + dx4[d], ny = y + dy4[d]; if (nx >= 0 && ny >= 0 && nx < w && ny < h && !m[ny * w + nx] && dist[ny * w + nx] > dist[c] + 1) { dist[ny * w + nx] = dist[c] + 1; q[qt++] = ny * w + nx; } }
  }
}
void movePl(const uint8_t *m, int w, int h, float &x, float &y, float dx, float dy, float r) {
  float nx = x + dx;
  if (!solidG(m, w, h, nx + r, y + r) && !solidG(m, w, h, nx - r, y + r) && !solidG(m, w, h, nx + r, y - r) && !solidG(m, w, h, nx - r, y - r)) x = nx;
  float ny = y + dy;
  if (!solidG(m, w, h, x + r, ny + r) && !solidG(m, w, h, x - r, ny + r) && !solidG(m, w, h, x + r, ny - r) && !solidG(m, w, h, x - r, ny - r)) y = ny;
}
void wkSet(Wk &k, float x, float y) { k.x = x; k.y = y; k.cx = k.tx = (int)x; k.cy = k.ty = (int)y; }
bool wkStep(Wk &k, const uint8_t *m, int w, int h, const int16_t *dist, float spd, float dt) {
  float tx = k.tx + 0.5f, ty = k.ty + 0.5f, dx = tx - k.x, dy = ty - k.y, d = sqrtf(dx * dx + dy * dy);
  if (d < 0.05f) {
    k.cx = k.tx; k.cy = k.ty;
    int best = dist[k.cy * w + k.cx]; if (best == 0) return true;
    static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    for (int i = 0; i < 4; i++) { int nx = k.cx + dx4[i], ny = k.cy + dy4[i]; if (nx >= 0 && ny >= 0 && nx < w && ny < h && dist[ny * w + nx] < best) { best = dist[ny * w + nx]; k.tx = nx; k.ty = ny; } }
    return false;
  }
  float st = spd * dt; if (st > d) st = d; k.x += dx / d * st; k.y += dy / d * st; return false;
}
void fxNoise(int n) { for (int i = 0; i < n; i++) { uint8_t g = random(30, 200); FB[random(SW * SH)] = RGB(g, g, g); } }
void fxVig(float k) {
  int th = (int)(k * 12); if (th < 1) return;
  for (int y = 0; y < SH; y++) {
    int d = y < SH - 1 - y ? y : SH - 1 - y;
    if (d < th) { for (int x = 0; x < SW; x++) FB[y * SW + x] = redshift(FB[y * SW + x]); }
    else for (int x = 0; x < th; x++) { FB[y * SW + x] = redshift(FB[y * SW + x]); FB[y * SW + SW - 1 - x] = redshift(FB[y * SW + SW - 1 - x]); }
  }
}
void fxGlitch(int n) {
  for (int i = 0; i < n; i++) { int y = random(SH), off = random(-9, 10), hgt = random(1, 4); for (int yy = y; yy < y + hgt && yy < SH; yy++) for (int x = 0; x < SW; x++) { int sx = x + off; if (sx >= 0 && sx < SW) FB[yy * SW + x] = FB[yy * SW + sx]; } }
}

// ---- وشوش مرعبة (رسم تحليلي): 0..2 بورتريه هادي | 3 وش شاحب بابتسامة ممزقة | 4 جمجمة بعيون فاضية ----
uint16_t faceCol(float fu, float fv, int var, int br, uint32_t sd) {
  float dx = fu - 0.5f;
  if (var < 3) {
    if (inEll(fu, fv, 0.5f, 0.44f, 0.26f, 0.34f)) {
      if (fv < 0.20f) return shc(40, 28, 20, br);
      if (inEll(fu, fv, 0.39f, 0.42f, 0.05f, 0.03f) || inEll(fu, fv, 0.61f, 0.42f, 0.05f, 0.03f)) return (inEll(fu, fv, 0.39f, 0.42f, 0.02f, 0.025f) || inEll(fu, fv, 0.61f, 0.42f, 0.02f, 0.025f)) ? shc(30, 30, 40, br) : shc(240, 240, 240, br);
      if (fabsf(dx) < 0.10f && fabsf(fv - (0.62f + dx * dx * 5.0f)) < 0.012f) return shc(150, 50, 50, br);
      if (fabsf(dx) < 0.015f && fv > 0.45f && fv < 0.56f) return shc(200, 150, 125, br);
      return shc(228, 190, 160, br);
    }
    if (fv > 0.78f) return shc(50 + var * 25, 55, 90, br);
    return var == 0 ? shc(70, 100, 120, br) : (var == 1 ? shc(110, 90, 70, br) : shc(80, 110, 80, br));
  }
  if (var == 3) {
    if (inEll(fu, fv, 0.5f, 0.46f, 0.34f, 0.44f)) {
      if (inEll(fu, fv, 0.35f, 0.36f, 0.11f, 0.13f)) return inEll(fu, fv, 0.36f, 0.38f, 0.025f, 0.04f) ? RGB(255, 20, 20) : RGB(4, 0, 0);
      if (inEll(fu, fv, 0.66f, 0.33f, 0.08f, 0.09f)) return inEll(fu, fv, 0.66f, 0.34f, 0.02f, 0.03f) ? RGB(255, 20, 20) : RGB(4, 0, 0);
      if (fv > 0.40f && fabsf(fu - 0.35f) < 0.012f && fv < 0.62f + (hh(sd) & 7) * 0.03f) return RGB(170, 0, 0);
      if (fv > 0.38f && fabsf(fu - 0.66f) < 0.01f && fv < 0.55f) return RGB(170, 0, 0);
      if (inEll(fu, fv, 0.5f, 0.72f, 0.22f, 0.13f)) { if ((fv < 0.67f || fv > 0.78f) && (((int)(fu * 26)) & 1)) return shc(220, 215, 190, br); return RGB(12, 0, 0); }
      if (inEll(fu, fv, 0.47f, 0.55f, 0.02f, 0.03f) || inEll(fu, fv, 0.53f, 0.55f, 0.02f, 0.03f)) return C_K;
      if ((((int)(fv * 60)) + sd) % 9 == 0) return shc(150, 150, 140, br);
      return shc(205, 205, 190, br);
    }
    return fv < 0.3f ? RGB(4, 2, 2) : shc(28, 0, 0, br);
  }
  if (inEll(fu, fv, 0.5f, 0.46f, 0.30f, 0.42f) || inEll(fu, fv, 0.5f, 0.75f, 0.18f, 0.14f)) {
    if (inEll(fu, fv, 0.38f, 0.40f, 0.09f, 0.10f) || inEll(fu, fv, 0.62f, 0.40f, 0.09f, 0.10f)) return (inEll(fu, fv, 0.40f, 0.38f, 0.015f, 0.02f) || inEll(fu, fv, 0.64f, 0.38f, 0.015f, 0.02f)) ? RGB(255, 255, 255) : C_K;
    if (fv > 0.52f && fv < 0.64f && fabsf(dx) < (fv - 0.52f) * 0.25f) return C_K;
    if (fv > 0.70f && fv < 0.86f && fabsf(dx) < 0.17f) return (((int)(fu * 30)) & 1) ? shc(235, 230, 205, br) : C_K;
    return shc(225, 220, 195, br);
  }
  return shc(6, 6, 8, br);
}
void scareDraw(int var, float k) {
  float z = 0.55f + 0.6f * fminf(1, k * 5), jx = random(-4, 5) / 100.0f, jy = random(-4, 5) / 100.0f;
  int br = random(150, 256); uint32_t sd = random(1000);
  for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
    float fu = 0.5f + (x / 127.0f - 0.5f) / z + jx, fv = 0.5f + (y / 127.0f - 0.5f) / z + jy;
    FB[y * SW + x] = (fu < 0 || fu > 1 || fv < 0 || fv > 1) ? RGB(2, 0, 0) : faceCol(fu, fv, var, br, sd);
  }
  fxNoise(900); fxGlitch(6);
  if ((millis() / 70) & 1) for (int y = 0; y < SH; y += 2) hl(0, SW - 1, y, mix(FB[y * SW], RGB(255, 0, 0), 80));
}

// ======================= القصص الافتتاحية (أنيميشن + نص) =======================
#define ST_LINE 3.0f
const char *const STORY[3][6] = {   // كل مشهد سطرين (يتفصلوا بـ |)
  {"1987. VOLTEX FACTORY|BURNED DOWN. 12 LOST.", "TONIGHT YOUR BROTHER|CALLED. HE WAS SCARED", "HE SAID: THE POWER IS|OFF. DON'T COME IN.", "YOU CAME ANYWAY.|SOMETHING IS INSIDE.", "FIND 3 FUSES TO OPEN|THE EXIT. AND RUN.", nullptr},
  {"METRO STATION 13.|LAST TRAIN LEFT 1AM.", "YOU ARE THE NIGHT|GUARD. ALWAYS ALONE.", "ONE RULE: CHECK THE|CORRIDOR EVERY LOOP.", "SEE SOMETHING WRONG?|TURN BACK. IT'S REAL.", "NOTHING WRONG? GO ON.|8 RIGHT = YOU LEAVE.", nullptr},
  {"NIGHT. HIGHWAY 9.|YOU DRIVE HOME ALONE.", "A TALL MAN STANDS IN|THE MIDDLE OF ROAD.", "YOU SWERVE. TOO LATE.|THE TREES ARE CLOSE.", "THE CAR IS DEAD.|PARTS ARE SCATTERED.", "FIND 5 PARTS IN THE|WOODS & REPAIR IT.", "HE HUNTS WHAT MOVES.|STOP + LIGHT OFF=SAFE"}};
const uint8_t STN[3] = {5, 5, 6};
int stId = 0, stLast = -1, stFlag = 0; float stT = 0;
void storyStart(int id) { lutInit(); stBuild(id); stId = id; stT = 0; stLast = -1; stFlag = 0; audioFlush(); }
bool storyUpd(float dt) {
  noPause = true; stT += dt;
  int line = (int)(stT / ST_LINE);
  if (line != stLast) {
    stLast = line;
    if (stId == 0 && line == 3) SFX(SFX_GROWL);
    if (stId == 1 && line == 3) SFX(SFX_CREAK);
    if (stId == 1 && line == 4) SFX(SFX_HEART);
    if (stId == 2 && line == 1) SFX(SFX_HEART);
    if (stId == 2 && line == 3) SFXL(SFX_CREAK);
  }
  if (stId == 2 && stT >= 7.4f && stFlag == 0) { stFlag = 1; SFX(SFX_BOOM); shake = 0.5f; }
  { static float pS = 0; float sv = sinf(stT * (stId == 1 ? 6.5f : 8.0f)); bool wk = (stId == 0 && stT < 9.8f) || stId == 1 || (stId == 2 && stT > 12.0f); if (wk && sv > 0 && pS <= 0) sndL(stId == 1 ? 130 : 105, 16); pS = sv; }
  if (stT > 0.6f && (prs[K_ST] || prs[K_A])) return true;
  return line >= STN[stId];
}
void svMenuDraw(const char *title, const char *info) {
  vgrad(0, SH, RGB(8, 4, 14), RGB(30, 6, 12)); starsDraw(0.1f);
  txtC(title, 20, RGB(230, 60, 60), 2);
  txtC("SAVE FOUND", 52, RGB(255, 220, 80)); txtC(info, 64, C_W);
  txtC("A: CONTINUE", 84, RGB(120, 255, 160)); txtC("B: NEW GAME", 98, RGB(255, 120, 120));
}

// ---------- 11) CRAFT 3D (عالم مفتوح: تضاريس + كهوف + مياه + أشجار + خامات + نهار/ليل + حيوانات + حفظ) ----------
#define CW 48
#define CD 48
#define CH 24
#define SEA 8
#define CW_ME 700
#define CI(x, y, z) ((((y) * CD) + (z)) * CW + (x))
uint8_t cwm[CH * CD * CW];
uint16_t edIdx[CW_ME]; uint8_t edB[CW_ME]; int edN = 0;
uint32_t cwSeed;
float cpx, cpy, cpz, cvx, cvy, cvz, cyaw, cpit, ctm, cmine, crSaveT, cTurn, cBob, cMsgT;
bool cground, cfly, cwater, cheadW, cStUsed, cStPrev;
int cslot, cmx, cmy, cmz, cplx, cply, cplz, cday; bool cTarget; const char *cMsg = "";
uint8_t cinv[16];
float czb[64 * 64], gFx, gFy, gFz, gRx, gRz, gUx, gUy, gUz, gEx, gEy, gEz, cLight = 1;
struct Pig { float x, y, z, a, t; bool mv; } pigs[6];
const uint8_t HB[9] = {2, 3, 4, 8, 9, 10, 6, 5, 14};
const char *BNAME[16] = {"", "GRASS", "DIRT", "STONE", "WOOD", "LEAVES", "SAND", "WATER", "PLANKS", "BRICK", "GLASS", "COAL", "GOLD", "SNOW", "GLOW", "BEDROCK"};
const float BHARD[16] = {0, .5f, .4f, 1.6f, 1.0f, .15f, .35f, 0, .8f, 1.8f, .3f, 2.0f, 2.2f, .3f, .4f, 1e9f};
struct CwSave { uint32_t magic, seed; float px, py, pz, yaw, pit, tm; int score; uint8_t inv[16]; uint8_t fly, slot; uint16_t n; uint16_t idx[CW_ME]; uint8_t b[CW_ME]; };
CwSave csv;

float vnz(int x, int z, int sc, uint32_t sd) {
  float fx = (float)x / sc, fz = (float)z / sc; int ix = (int)floorf(fx), iz = (int)floorf(fz); float tx = fx - ix, tz = fz - iz;
  tx = tx * tx * (3 - 2 * tx); tz = tz * tz * (3 - 2 * tz);
  float a = hh((uint32_t)ix * 374761393u + (uint32_t)iz * 668265263u + sd * 1442695041u) / 255.0f;
  float b = hh((uint32_t)(ix + 1) * 374761393u + (uint32_t)iz * 668265263u + sd * 1442695041u) / 255.0f;
  float c = hh((uint32_t)ix * 374761393u + (uint32_t)(iz + 1) * 668265263u + sd * 1442695041u) / 255.0f;
  float d = hh((uint32_t)(ix + 1) * 374761393u + (uint32_t)(iz + 1) * 668265263u + sd * 1442695041u) / 255.0f;
  return a + (b - a) * tx + (c - a) * tz + (a - b - c + d) * tx * tz;
}
static inline uint8_t cwGet(int x, int y, int z) {
  if (y >= CH) return 0; if (y < 0) return 15;
  if ((unsigned)x >= CW || (unsigned)z >= CD) return 15;
  return cwm[CI(x, y, z)];
}
static inline bool cSolid(int x, int y, int z) { uint8_t b = cwGet(x, y, z); return b != 0 && b != 7; }
int cwTopY(int x, int z) { for (int y = CH - 1; y >= 0; y--) { uint8_t b = cwm[CI(x, y, z)]; if (b && b != 7) return y; } return -1; }
void cwSet(int x, int y, int z, uint8_t b) {
  if ((unsigned)x >= CW || (unsigned)z >= CD || (unsigned)y >= CH) return;
  int id = CI(x, y, z); cwm[id] = b;
  for (int i = 0; i < edN; i++) if (edIdx[i] == id) { edB[i] = b; return; }
  if (edN < CW_ME) { edIdx[edN] = id; edB[edN] = b; edN++; }
}
void cwGen(uint32_t sd) {
  cwSeed = sd; memset(cwm, 0, sizeof(cwm)); uint32_t sm = sd & 63;
  for (int z = 0; z < CD; z++) for (int x = 0; x < CW; x++) {
    float n = vnz(x, z, 18, sd) * 0.6f + vnz(x, z, 8, sd + 7) * 0.3f + vnz(x, z, 4, sd + 13) * 0.1f;
    int h = 4 + (int)(n * 17.0f); if (h > CH - 5) h = CH - 5;
    for (int y = 0; y <= h; y++) {
      uint8_t b;
      if (y == 0) b = 15;
      else if (y < h - 3) {
        uint32_t r = hh((x * 73 + z * 131 + y * 17) * 977u + sd) % 100; b = 3;
        if (y < 9 && r < 1) b = 12; else if (r < 4) b = 11;
        float cvv = sinf(x * 0.31f + y * 0.17f + sm) * sinf(z * 0.29f + y * 0.23f) * sinf((x + z) * 0.13f + y * 0.37f);
        if (y > 2 && cvv > 0.52f) b = 0;
      } else if (y < h) b = (h <= SEA + 1) ? 6 : 2;
      else b = (h <= SEA + 1) ? 6 : (h >= 16 ? 13 : 1);
      cwm[CI(x, y, z)] = b;
    }
    for (int y = h + 1; y <= SEA; y++) cwm[CI(x, y, z)] = 7;
  }
  for (int z = 3; z < CD - 3; z++) for (int x = 3; x < CW - 3; x++) {
    int ty = cwTopY(x, z); if (ty < 0 || cwm[CI(x, ty, z)] != 1) continue;
    if (hh(x * 31 + z * 57 + sd) % 100 >= 3) continue;
    int th = 4 + (hh(x * 7 + z * 3) & 1); if (ty + th + 3 >= CH) continue;
    for (int t = 1; t <= th; t++) cwm[CI(x, ty + t, z)] = 4;
    for (int dy = th - 1; dy <= th + 1; dy++) { int r = dy == th + 1 ? 1 : 2; for (int dz = -r; dz <= r; dz++) for (int dx = -r; dx <= r; dx++) {
      if (r == 2 && abs(dx) == 2 && abs(dz) == 2) continue; if (!cwm[CI(x + dx, ty + dy, z + dz)]) cwm[CI(x + dx, ty + dy, z + dz)] = 5; } }
  }
}
bool cBox(float x, float y, float z) {
  const float r = 0.28f; int x0 = (int)floorf(x - r), x1 = (int)floorf(x + r), z0 = (int)floorf(z - r), z1 = (int)floorf(z + r), y0 = (int)floorf(y), y1 = (int)floorf(y + 1.74f);
  for (int yy = y0; yy <= y1; yy++) for (int zz = z0; zz <= z1; zz++) for (int xx = x0; xx <= x1; xx++) if (cSolid(xx, yy, zz)) return true;
  return false;
}
void cwSpawnPlayer() {
  int bx = CW / 2, bz = CD / 2, best = -1;
  for (int r = 0; r < 20 && best < 0; r++) for (int dz = -r; dz <= r && best < 0; dz++) for (int dx = -r; dx <= r; dx++) {
    int x = bx + dx, z = bz + dz; if ((unsigned)x >= CW || (unsigned)z >= CD) continue; int ty = cwTopY(x, z);
    if (ty > SEA + 1 && cwm[CI(x, ty, z)] == 1) { cpx = x + 0.5f; cpz = z + 0.5f; cpy = ty + 1.01f; best = 1; break; }
  }
  if (best < 0) { cpx = CW / 2; cpz = CD / 2; cpy = CH - 2; }
}
void crSave() {
  csv.magic = 0x43570002; csv.seed = cwSeed; csv.px = cpx; csv.py = cpy; csv.pz = cpz; csv.yaw = cyaw; csv.pit = cpit; csv.tm = ctm; csv.score = score;
  memcpy(csv.inv, cinv, 16); csv.fly = cfly; csv.slot = cslot; csv.n = edN;
  for (int i = 0; i < edN; i++) { csv.idx[i] = edIdx[i]; csv.b[i] = edB[i]; }
  pref.putBytes("cw", &csv, sizeof(csv));
  if (score > hiS[GM_CRAFT]) { hiS[GM_CRAFT] = score; pref.putUInt("h10", score); }
  cMsg = "SAVED"; cMsgT = 1.2f;
}
void crInit() {
  cmine = 0; cslot = 0; cvx = cvy = cvz = 0; cTurn = 0; cStPrev = false; cStUsed = false; cfly = false; cMsgT = 0; crSaveT = 0; cTarget = false; edN = 0;
  memset(cinv, 0, 16); for (int i = 0; i < 9; i++) cinv[HB[i]] = 12;
  bool ok = pref.getBytes("cw", &csv, sizeof(csv)) == sizeof(csv) && csv.magic == 0x43570002;
  if (ok) {
    cwGen(csv.seed);
    for (int i = 0; i < csv.n && i < CW_ME; i++) if (csv.idx[i] < CH * CD * CW) { cwm[csv.idx[i]] = csv.b[i]; edIdx[edN] = csv.idx[i]; edB[edN] = csv.b[i]; edN++; }
    cpx = csv.px; cpy = csv.py; cpz = csv.pz; cyaw = csv.yaw; cpit = csv.pit; ctm = csv.tm; score = csv.score; memcpy(cinv, csv.inv, 16); cfly = csv.fly; cslot = csv.slot % 9;
  } else { cwGen(micros() ^ (uint32_t)random(100000)); cwSpawnPlayer(); cyaw = 0.8f; cpit = -0.1f; ctm = 25; }
  for (auto &p : pigs) { for (int t = 0; t < 40; t++) { int x = random(4, CW - 4), z = random(4, CD - 4), ty = cwTopY(x, z); if (ty > SEA + 1 && cwm[CI(x, ty, z)] == 1) { p.x = x + 0.5f; p.z = z + 0.5f; p.y = ty + 1; break; } p.x = CW / 2; p.z = CD / 2; p.y = CH; } p.a = random(628) / 100.0f; p.t = 1; p.mv = false; }
}
bool cwPick() {
  float cp = cosf(cpit), fx = cosf(cyaw) * cp, fy = sinf(cpit), fz = sinf(cyaw) * cp;
  float ex = cpx, ey = cpy + 1.6f, ez = cpz; int mx = (int)floorf(ex), my = (int)floorf(ey), mz = (int)floorf(ez);
  float ddx = fx == 0 ? 1e30f : fabsf(1 / fx), ddy = fy == 0 ? 1e30f : fabsf(1 / fy), ddz = fz == 0 ? 1e30f : fabsf(1 / fz);
  int sx = fx < 0 ? -1 : 1, sy = fy < 0 ? -1 : 1, sz = fz < 0 ? -1 : 1;
  float tx = fx < 0 ? (ex - mx) * ddx : (mx + 1 - ex) * ddx, ty = fy < 0 ? (ey - my) * ddy : (my + 1 - ey) * ddy, tz = fz < 0 ? (ez - mz) * ddz : (mz + 1 - ez) * ddz;
  for (int s = 0; s < 14; s++) {
    int pmx = mx, pmy = my, pmz = mz; float t;
    if (tx < ty && tx < tz) { mx += sx; t = tx; tx += ddx; } else if (ty < tz) { my += sy; t = ty; ty += ddy; } else { mz += sz; t = tz; tz += ddz; }
    if (t > 5.5f) return false;
    if ((unsigned)mx >= CW || (unsigned)mz >= CD || my < 0 || my >= CH) { if (my >= CH && fy > 0) return false; if (my < 0) return false; return false; }
    uint8_t b = cwm[CI(mx, my, mz)];
    if (b && b != 7) { cmx = mx; cmy = my; cmz = mz; cplx = pmx; cply = pmy; cplz = pmz; return true; }
  }
  return false;
}
void cwTex(uint8_t b, int face, int tu, int tv, uint32_t hs, int &r, int &g, int &bl) {
  int n = (int)(hh(hs + tu * 7 + tv * 13) & 31) - 16;
  switch (b) {
    case 1: if (face == 0) { r = 64 + n; g = 150 + n; bl = 48; } else if (face == 1 || tv > 2 || (tv == 2 && !(tu & 1))) { r = 118 + n; g = 84 + n; bl = 54 + n; } else { r = 64 + n; g = 150 + n; bl = 48; } break;
    case 2: r = 118 + n; g = 84 + n; bl = 54 + n; break;
    case 3: r = 125 + n; g = 125 + n; bl = 128 + n; break;
    case 4: if (face != 2) { int d = max(abs(2 * tu - 7), abs(2 * tv - 7)) >> 1; r = (d & 1) ? 150 : 124; g = (d & 1) ? 112 : 88; bl = 66; } else { r = 100 + n + ((tu & 2) ? 12 : 0); g = 74 + n / 2; bl = 44; } break;
    case 5: if ((hh(hs + tu * 5 + tv * 3) & 7) == 0) { r = 20; g = 70; bl = 25; } else { r = 40 + n; g = 125 + n; bl = 40; } break;
    case 6: r = 226 + n / 2; g = 210 + n / 2; bl = 150 + n / 2; break;
    case 7: r = 40 + n / 2; g = 100 + n / 2; bl = 210; break;
    case 8: r = 172 + n / 2; g = 132 + n / 2; bl = 82; if ((tv & 3) == 3) { r -= 40; g -= 35; bl -= 25; } break;
    case 9: if ((tv & 3) == 3 || (((tu + (((tv >> 2) & 1) * 4)) & 7) == 7)) { r = 190; g = 185; bl = 170; } else { r = 160 + n; g = 70 + n / 2; bl = 55; } break;
    case 10: r = 200; g = 235; bl = 245; break;
    case 11: if (hh(hs + tu * 11 + tv * 3) % 5 == 0) { r = 25; g = 25; bl = 28; } else { r = 125 + n; g = 125 + n; bl = 128 + n; } break;
    case 12: if (hh(hs + tu * 11 + tv * 3) % 5 == 0) { r = 240; g = 200; bl = 40; } else { r = 125 + n; g = 125 + n; bl = 128 + n; } break;
    case 13: r = 240 + n / 3; g = 244; bl = 250; break;
    case 14: r = 255; g = 225 + n / 2; bl = 130; if (tu == 0 || tv == 0) { r = 220; g = 180; bl = 90; } break;
    default: r = 60 + n; g = 60 + n; bl = 64 + n; break;
  }
}
void rectD(int x0, int y0, int w, int h, uint16_t c, float dpt) {
  for (int y = y0; y < y0 + h; y++) { if (y < 0 || y >= SH) continue; for (int x = x0; x < x0 + w; x++) { if (x < 0 || x >= SW) continue; if (dpt < czb[(y >> 1) * 64 + (x >> 1)]) FB[y * SW + x] = c; } }
}
float cSunX, cSunY, cSunZ, cDayK; int cHzR, cHzG, cHzB, cZeR, cZeG, cZeB, cLv;
void crRows(int p0, int p1) {
  const float K = 0.9f, FOGD = 22.0f;
  float fx = gFx, fy = gFy, fz = gFz, rx = gRx, rz = gRz, ux = gUx, uy = gUy, uz = gUz, ex = gEx, ey = gEy, ez = gEz;
  float sunX = cSunX, sunY = cSunY, sunZ = cSunZ, dayK = cDayK; int lv = cLv, hzR = cHzR, hzG = cHzG, hzB = cHzB, zeR = cZeR, zeG = cZeG, zeB = cZeB;
  for (int py = p0; py < p1; py++) {
    float v = (1.0f - (py + 0.5f) / 32.0f) * K;
    for (int pxx = 0; pxx < 64; pxx++) {
      float u = ((pxx + 0.5f) / 32.0f - 1.0f) * K;
      float dx = fx + rx * u + ux * v, dy = fy + uy * v, dz = fz + rz * u + uz * v;
      float len = sqrtf(dx * dx + dy * dy + dz * dz);
      int mx = (int)floorf(ex), my = (int)floorf(ey), mz = (int)floorf(ez);
      float ddx = dx == 0 ? 1e30f : fabsf(1.0f / dx), ddy = dy == 0 ? 1e30f : fabsf(1.0f / dy), ddz = dz == 0 ? 1e30f : fabsf(1.0f / dz);
      int stx = dx < 0 ? -1 : 1, sty = dy < 0 ? -1 : 1, stz = dz < 0 ? -1 : 1;
      float tx = dx < 0 ? (ex - mx) * ddx : (mx + 1 - ex) * ddx, ty = dy < 0 ? (ey - my) * ddy : (my + 1 - ey) * ddy, tz = dz < 0 ? (ez - mz) * ddz : (mz + 1 - ez) * ddz;
      int hitB = 0, axis = 0, steps = 0, face = 2, tu = 0, tv = 0; float t = 0;
      while (steps++ < 40) {
        if (tx < ty && tx < tz) { mx += stx; t = tx; tx += ddx; axis = 0; } else if (ty < tz) { my += sty; t = ty; ty += ddy; axis = 1; } else { mz += stz; t = tz; tz += ddz; axis = 2; }
        if (t > 24.0f || (unsigned)mx >= CW || (unsigned)mz >= CD || my < 0 || (my >= CH && dy > 0)) break;
        if (my >= CH) continue;
        uint8_t b = cwm[CI(mx, my, mz)];
        if (!b) continue;
        float hx = ex + dx * t, hy = ey + dy * t, hz = ez + dz * t, fu, fv;
        if (axis == 1) { face = dy > 0 ? 1 : 0; fu = hx - floorf(hx); fv = hz - floorf(hz); }
        else { face = 2; fu = axis == 0 ? hz - floorf(hz) : hx - floorf(hx); fv = 1.0f - (hy - floorf(hy)); }
        tu = (int)(fu * 8) & 7; tv = (int)(fv * 8) & 7;
        if (b == 10 && tu > 0 && tu < 7 && tv > 0 && tv < 7) continue;
        hitB = b; break;
      }
      uint16_t col;
      if (hitB) {
        int r, g, bl; cwTex(hitB, face, tu, tv, (uint32_t)(mx * 7 + my * 13 + mz * 29), r, g, bl);
        if (hitB == 7 && face == 0) { float w = sinf(millis() * 0.003f + mx * 1.3f + mz * 0.9f) * 14; r += (int)w; g += (int)w; }
        int sh = face == 0 ? 255 : (face == 1 ? 130 : (axis == 0 ? 205 : 170)), k = hitB == 14 ? 255 : (sh * lv) >> 8;
        if (cTarget && mx == cmx && my == cmy && mz == cmz) { r += 45; g += 45; bl += 45; }
        r = (r * k) >> 8; g = (g * k) >> 8; bl = (bl * k) >> 8;
        float fg = t * len / FOGD; fg = fg * fg; if (fg > 1) fg = 1; int fi = (int)(fg * 255);
        r += ((hzR - r) * fi) >> 8; g += ((hzG - g) * fi) >> 8; bl += ((hzB - bl) * fi) >> 8;
        col = RGB(r < 0 ? 0 : (r > 255 ? 255 : r), g < 0 ? 0 : (g > 255 ? 255 : g), bl < 0 ? 0 : (bl > 255 ? 255 : bl));
        czb[py * 64 + pxx] = t;
      } else {
        float inv = 1.0f / len, ndx = dx * inv, ndy = dy * inv, ndz = dz * inv, s = constrain(ndy * 1.8f, 0.0f, 1.0f);
        int r = hzR + (int)((zeR - hzR) * s), g = hzG + (int)((zeG - hzG) * s), bl = hzB + (int)((zeB - hzB) * s);
        float sd = ndx * sunX + ndy * sunY + ndz * sunZ;
        if (sd > 0.9965f) { r = 255; g = 245; bl = 190; } else if (sd > 0.985f && dayK > 0.1f) { float q = (sd - 0.985f) / 0.0115f; r += (int)(70 * q); g += (int)(60 * q); bl += (int)(15 * q); }
        else if (-sd > 0.9975f && dayK < 0.7f) { r = 235; g = 235; bl = 255; }
        if (dayK < 0.5f && ndy > 0.05f) { uint32_t q = (uint32_t)((int)(ndx * 70 + 100)) * 7919u + (uint32_t)((int)(ndy * 70 + 100)) * 104729u + (uint32_t)((int)(ndz * 70 + 100)) * 1299709u; if (hh(q) < 3) { int a = (int)(200 * (1 - dayK * 2)); r += a; g += a; bl += a; } }
        col = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, bl > 255 ? 255 : bl);
        czb[py * 64 + pxx] = 1e9f;
      }
      uint16_t *o = FB + (2 * py) * SW + 2 * pxx; o[0] = col; o[1] = col; o[SW] = col; o[SW + 1] = col;
    }
  }
}
void crRender() {
  float cp = cosf(cpit), sp = sinf(cpit), cyw = cosf(cyaw), syw = sinf(cyaw);
  gFx = cyw * cp; gFy = sp; gFz = syw * cp; gRx = -syw; gRz = cyw; gUx = -sp * cyw; gUy = cp; gUz = -sp * syw;
  gEx = cpx; gEy = cpy + 1.6f + cBob; gEz = cpz;
  float sa = ctm * 6.2831853f / 240.0f; cSunY = sinf(sa); cSunX = cosf(sa); cSunZ = 0.35f;
  float sn = sqrtf(cSunX * cSunX + cSunY * cSunY + cSunZ * cSunZ); cSunX /= sn; cSunY /= sn; cSunZ /= sn;
  cDayK = constrain((cSunY + 0.18f) / 0.45f, 0.0f, 1.0f); cLight = 0.42f + 0.58f * cDayK; cLv = (int)(cLight * 255);
  cHzR = 14 + (int)(151 * cDayK); cHzG = 18 + (int)(192 * cDayK); cHzB = 44 + (int)(211 * cDayK);
  cZeR = 3 + (int)(57 * cDayK); cZeG = 5 + (int)(115 * cDayK); cZeB = 20 + (int)(215 * cDayK);
  parRun(crRows, 0, 32, 64);
  if (cheadW) for (int i = 0; i < SW * SH; i++) FB[i] = mix(FB[i], RGB(20, 60, 200), 90);
}
void crPigs() {
  const float K = 0.9f;
  for (auto &p : pigs) {
    float rx0 = p.x - gEx, ry0 = p.y + 0.4f - gEy, rz0 = p.z - gEz;
    float dpt = rx0 * gFx + ry0 * gFy + rz0 * gFz; if (dpt < 0.4f || dpt > 20) continue;
    float u = (rx0 * gRx + rz0 * gRz) / (dpt * K), v = (rx0 * gUx + ry0 * gUy + rz0 * gUz) / (dpt * K);
    int sx = (int)((1 + u) * 64), sy = (int)((1 - v) * 64); float ppb = 64.0f / (dpt * K);
    int pw = (int)(0.85f * ppb), ph = (int)(0.5f * ppb); if (pw < 2) pw = 2; if (ph < 2) ph = 2;
    int side = sinf(p.a - cyaw) > 0 ? 1 : -1, lg = ph * 6 / 10; if (lg < 1) lg = 1;
    uint16_t body = mix(RGB(245, 160, 170), C_K, 255 - (int)(cLight * 255)), dk = mix(RGB(190, 100, 112), C_K, 255 - (int)(cLight * 255)), sn = mix(RGB(255, 195, 205), C_K, 255 - (int)(cLight * 255));
    rectD(sx - pw / 2, sy - ph / 2 + 0, pw, ph, body, dpt);
    rectD(sx - pw / 2 + 1, sy + ph / 2, pw / 5 + 1, lg, dk, dpt); rectD(sx + pw / 2 - pw / 5 - 2, sy + ph / 2, pw / 5 + 1, lg, dk, dpt);
    int hw = pw * 4 / 10, hx = side > 0 ? sx + pw / 2 - 1 : sx - pw / 2 - hw + 1;
    rectD(hx, sy - ph / 2 - ph / 6, hw, ph * 8 / 10, body, dpt); rectD(side > 0 ? hx + hw - 1 : hx - 1, sy - ph / 8, hw / 2 + 1, ph / 3 + 1, sn, dpt);
    rectD(side > 0 ? hx + hw / 2 : hx + hw / 4, sy - ph / 3, 1 + ph / 8, 1 + ph / 8, C_K, dpt);
  }
}
void crPigUpd(float dt) {
  for (auto &p : pigs) {
    p.t -= dt; if (p.t <= 0) { p.t = 1 + random(30) / 10.0f; p.a += (random(300) - 150) / 100.0f; p.mv = random(100) < 65; }
    if (p.mv) {
      float nx = p.x + cosf(p.a) * 0.8f * dt, nz = p.z + sinf(p.a) * 0.8f * dt; int bx = (int)nx, bz = (int)nz, by = (int)p.y;
      if ((unsigned)bx >= CW || (unsigned)bz >= CD || cSolid(bx, by, bz) || cSolid(bx, by + 1, bz)) { p.a += 2.4f; p.mv = false; }
      else { int gy = -1; for (int y = min(CH - 1, by + 1); y >= 0; y--) if (cSolid(bx, y, bz)) { gy = y + 1; break; } if (gy < 0 || cwGet(bx, gy, bz) == 7 || cwGet(bx, gy - 1, bz) == 7 || gy > p.y + 1.2f) { p.a += 2.4f; p.mv = false; } else { p.x = nx; p.z = nz; } }
    }
    int bx = (int)p.x, bz = (int)p.z; int gy = -1; for (int y = min(CH - 1, (int)p.y + 1); y >= 0; y--) if (cSolid(bx, y, bz)) { gy = y + 1; break; }
    if (gy >= 0) p.y += (gy - p.y) * fminf(1, dt * 10);
  }
}
void crUpd(float dt) {
  noPause = true; cMsgT -= dt; ctm += dt; crSaveT += dt; cday = (int)(ctm / 240) + 1;
  bool mod = held[K_ST];
  if (mod) cStUsed = cStUsed || prs[K_LF] || prs[K_RT] || prs[K_A] || held[K_UP] || held[K_DN];
  if (!mod && cStPrev && !cStUsed && cTarget) {   // تركيب بلوك
    uint8_t pb = cwGet(cplx, cply, cplz), it = HB[cslot];
    bool free_ = (pb == 0 || pb == 7) && cply >= 0 && cply < CH && (unsigned)cplx < CW && (unsigned)cplz < CD;
    bool inP = cplx + 1 > cpx - 0.28f && cplx < cpx + 0.28f && cplz + 1 > cpz - 0.28f && cplz < cpz + 0.28f && cply + 1 > cpy && cply < cpy + 1.74f;
    if (free_ && !inP && cinv[it] > 0) { cwSet(cplx, cply, cplz, it); cinv[it]--; snd(300, 40); } else if (cinv[it] == 0) { cMsg = "EMPTY"; cMsgT = 1; SFX(SFX_HIT); }
  }
  if (!mod) cStUsed = false; cStPrev = mod;
  if (mod) {
    cpit = constrain(cpit + ((held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0)) * 1.5f * sensM() * 0.8f * dt, -1.3f, 1.3f);
    if (prs[K_LF]) { cslot = (cslot + 8) % 9; sndL(900, 20); } if (prs[K_RT]) { cslot = (cslot + 1) % 9; sndL(900, 20); }
    if (prs[K_A]) { cfly = !cfly; cvy = 0; cMsg = cfly ? "FLY ON" : "FLY OFF"; cMsgT = 1; SFX(SFX_OK); }
  }
  float tin = mod ? 0 : ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0));
  cTurn += (tin * 2.3f * sensM() - cTurn) * fminf(1, dt * 14); cyaw += cTurn * dt;
  float mv = mod ? 0 : ((held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0));
  cwater = cwGet((int)floorf(cpx), (int)floorf(cpy + 0.4f), (int)floorf(cpz)) == 7;
  cheadW = cwGet((int)floorf(cpx), (int)floorf(cpy + 1.6f), (int)floorf(cpz)) == 7;
  float spd = cfly ? 8.0f : (cwater ? 2.2f : 4.2f); if (mv < 0) spd *= 0.6f;
  float cpp = cosf(cpit), tvx = cosf(cyaw) * mv * spd * (cfly ? cpp : 1), tvz = sinf(cyaw) * mv * spd * (cfly ? cpp : 1);
  float ac = cground || cfly || cwater ? 30.0f : 9.0f;
  cvx += (tvx - cvx) * fminf(1, ac * dt); cvz += (tvz - cvz) * fminf(1, ac * dt);
  if (cfly) { float tvy = sinf(cpit) * mv * spd + ((!mod && held[K_A]) ? 5.0f : 0); cvy += (tvy - cvy) * fminf(1, 10 * dt); }
  else if (cwater) { cvy -= 8 * dt; if (!mod && held[K_A]) cvy += 26 * dt; cvy *= (1 - 3 * dt); if (cvy > 3.5f) cvy = 3.5f; if (cvy < -4) cvy = -4; }
  else { cvy -= 24 * dt; if (cvy < -40) cvy = -40; if (!mod && held[K_A] && cground) { cvy = 7.8f; cground = false; sndL(500, 20); } }
  float nx = cpx + cvx * dt;
  if (!cBox(nx, cpy, cpz)) cpx = nx;
  else { if (mv > 0 && cground && !cBox(nx, cpy + 1.02f, cpz) && !cBox(cpx, cpy + 1.02f, cpz)) { cvy = 7.8f; cground = false; } cvx = 0; }
  float nz = cpz + cvz * dt;
  if (!cBox(cpx, cpy, nz)) cpz = nz;
  else { if (mv > 0 && cground && !cBox(cpx, cpy + 1.02f, nz) && !cBox(cpx, cpy + 1.02f, cpz)) { cvy = 7.8f; cground = false; } cvz = 0; }
  float ny = cpy + cvy * dt; cground = false;
  if (!cBox(cpx, ny, cpz)) cpy = ny;
  else { if (cvy < 0) { float sy = floorf(ny) + 1.001f; if (!cBox(cpx, sy, cpz)) cpy = sy; cground = true; } cvy = 0; }
  if (cpy < -5) { cwSpawnPlayer(); cvy = 0; }
  if (cpx < 0.4f) cpx = 0.4f; if (cpz < 0.4f) cpz = 0.4f; if (cpx > CW - 0.4f) cpx = CW - 0.4f; if (cpz > CD - 0.4f) cpz = CD - 0.4f;
  if ((fabsf(cvx) + fabsf(cvz)) > 0.5f && cground) cBob = sinf(millis() * 0.011f) * 0.05f; else cBob *= 0.85f;
  cTarget = cwPick();
  if (held[K_B] && !mod && cTarget) {
    uint8_t b = cwGet(cmx, cmy, cmz); cmine += dt / BHARD[b];
    if ((millis() & 127) < 20) burst(64 + random(-6, 7), 64 + random(-6, 7), RGB(150, 130, 100), 1, 30, 0.4f);
    if (cmine >= 1.0f) {
      cmine = 0; cwSet(cmx, cmy, cmz, 0);
      uint8_t d = b == 1 ? 2 : (b == 11 ? 3 : (b == 12 ? 14 : b));
      if (d != 13 && cinv[d] < 99) cinv[d]++;
      score += b == 12 ? 50 : (b == 11 ? 10 : (b == 3 ? 2 : 1));
      for (int k = 0; k < 8; k++) { int r, g, bl; cwTex(b, 2, random(8), random(8), k, r, g, bl); burst(64, 64, RGB(r, g, bl), 1, 60, 0.7f); }
      SFX(SFX_HIT);
    }
  } else cmine = 0;
  crPigUpd(dt);
  if (crSaveT > 45) { crSaveT = 0; crSave(); }
  float hrs = fmodf(ctm / 240.0f * 24.0f + 6.0f, 24.0f);
  snprintf(oledX, 24, "D%d %02d:%02d Y%d", cday, (int)hrs, (int)((hrs - (int)hrs) * 60), (int)cpy);
}
void crDraw() {
  crRender(); crPigs();
  cv.drawFastHLine(61, 64, 7, C_W); cv.drawFastVLine(64, 61, 7, C_W);
  if (cmine > 0.01f) { cv.drawRect(54, 72, 20, 4, C_W); cv.fillRect(55, 73, (int)(18 * cmine), 2, RGB(255, 200, 60)); }
  for (int i = 0; i < 9; i++) {
    int x = 6 + i * 13; int r, g, b; cwTex(HB[i], 0, 3, 3, 0, r, g, b);
    cv.fillRect(x, 114, 12, 12, RGB(26, 26, 30)); cv.fillRect(x + 2, 116, 8, 8, cinv[HB[i]] ? RGB(r, g, b) : RGB(r / 4, g / 4, b / 4));
    cv.drawRect(x, 114, 12, 12, i == cslot ? RGB(255, 230, 80) : RGB(90, 90, 100));
    if (i == cslot) cv.drawRect(x - 1, 113, 14, 14, RGB(255, 230, 80));
  }
  char b[28]; snprintf(b, 28, "%s x%d", BNAME[HB[cslot]], cinv[HB[cslot]]); txtS(b, (SW - (int)strlen(b) * 6) / 2, 103, C_W);
  float hrs = fmodf(ctm / 240.0f * 24.0f + 6.0f, 24.0f); snprintf(b, 28, "%02d:%02d", (int)hrs, (int)((hrs - (int)hrs) * 60)); txtS(b, 3, 3, C_W);
  snprintf(b, 28, "SC %d", score); txtS(b, SW - 3 - strlen(b) * 6, 3, RGB(255, 230, 80));
  if (cfly) txtS("FLY", 3, 13, RGB(120, 220, 255));
  if (cMsgT > 0) txtS(cMsg, (SW - (int)strlen(cMsg) * 6) / 2, 90, RGB(255, 255, 160));
  if (ctm < 40) txtS("START+<>:ITEM  START:PLACE", 4, 20, RGB(255, 255, 255));
}

// ---------- 12) HIDE & SEEK 3D (مرة انت بتستخبي ومرة انت بتدور - ضد عروسة غامضة) ----------
#define HSW 16
uint8_t hsm[HSW * HSW]; int16_t hsd[HSW * HSW];
struct Spot { int8_t x, y, fx, fy; };
Spot hsS[24]; int hsN;
float hsx, hsy, hsa, hsT, hsBreath, hsMsgT, hsHeartT, hsChk, hsLostT, hsBfsT, hsRel, hsBob; Wk hsK;
int hsRole, hsPh, hsHidSp, hsRound, hsGoal, hsAI; bool hsHid, hsWin, hsSprint; uint32_t hsVisited; const char *hsMsg = ""; float hsHidA;
uint16_t hsCL[256], hsFL[256]; bool hsLut = false;
void hsBuild() {
  memset(hsm, 1, sizeof(hsm)); hsN = 0;
  for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { int x0 = 5 * c + 1, y0 = 5 * r + 1; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) hsm[(y0 + y) * HSW + x0 + x] = 0; }
  for (int r = 0; r < 3; r++) for (int c = 0; c < 2; c++) { hsm[(5 * r + 2) * HSW + 5 * c + 5] = 0; hsm[(5 * r + 3) * HSW + 5 * c + 5] = 0; }
  for (int r = 0; r < 2; r++) for (int c = 0; c < 3; c++) if (c == 1 || ((c + r) & 1) == 0) { hsm[(5 * r + 5) * HSW + 5 * c + 2] = 0; hsm[(5 * r + 5) * HSW + 5 * c + 3] = 0; }
  for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
    int x0 = 5 * c + 1, y0 = 5 * r + 1;
    hsm[y0 * HSW + x0] = 2; hsS[hsN++] = {(int8_t)x0, (int8_t)y0, (int8_t)(x0 + 1), (int8_t)y0};
    hsm[(y0 + 3) * HSW + x0 + 3] = 2; hsS[hsN++] = {(int8_t)(x0 + 3), (int8_t)(y0 + 3), (int8_t)(x0 + 2), (int8_t)(y0 + 3)};
    hsm[(y0 + 1) * HSW + x0 + 1] = 3;
  }
}
uint16_t hsWall(int mx, int my, int side, float u, float v, int br) {
  int t = hsm[constrain(my, 0, HSW - 1) * HSW + constrain(mx, 0, HSW - 1)];
  if (t == 2) {
    if (u < 0.06f || u > 0.94f || v < 0.04f) return shc(70, 45, 25, br);
    if (fabsf(u - 0.5f) < 0.015f) return shc(60, 38, 22, br);
    if (inEll(u, v, 0.40f, 0.52f, 0.025f, 0.02f) || inEll(u, v, 0.60f, 0.52f, 0.025f, 0.02f)) return shc(235, 205, 80, br);
    bool pan = ((u > 0.1f && u < 0.46f) || (u > 0.54f && u < 0.9f)) && ((v > 0.1f && v < 0.45f) || (v > 0.55f && v < 0.95f));
    return pan ? shc(150, 102, 60, br) : shc(118, 78, 44, br);
  }
  if (t == 3) { if (v < 0.35f) return shc(60, 75, 130, br); if (v < 0.5f) return shc(225, 225, 235, br); return shc(95, 60, 40, br); }
  if (v > 0.74f) return shc(110, 72, 45, br);
  if (v > 0.71f) return shc(60, 40, 25, br);
  bool st = ((int)(u * 8)) & 1; if ((((int)(u * 16)) & 3) == 0 && (((int)(v * 10)) & 3) == 0) return shc(150, 120, 90, br);
  return st ? shc(214, 188, 146, br) : shc(196, 168, 128, br);
}
uint8_t dollShape(float u, float v) {
  float du = u - 0.5f, ad = fabsf(du);
  if (inEll(u, v, 0.5f, 0.15f, 0.12f, 0.1f)) { if (inEll(u, v, 0.44f, 0.15f, 0.025f, 0.03f) || inEll(u, v, 0.56f, 0.15f, 0.025f, 0.03f)) return 3; if (fabsf(du) < 0.03f && fabsf(v - 0.2f) < 0.01f) return 5; return 2; }
  if (inEll(u, v, 0.5f, 0.12f, 0.15f, 0.12f)) return 4;
  if (ad > 0.14f && ad < 0.2f && v > 0.12f && v < 0.3f) return 4;
  if (v >= 0.27f && v < 0.8f) { float w = 0.07f + (v - 0.27f) * 0.28f; if (ad < w) return 1; if (v < 0.55f && fabsf(ad - (0.09f + (v - 0.3f) * 0.1f)) < 0.025f) return 2; }
  if (v >= 0.8f && fabsf(ad - 0.05f) < 0.03f) return 2;
  return 0;
}
uint16_t dollCol(uint8_t k, int br) { switch (k) { case 1: return shc(190, 30, 50, br); case 2: return shc(235, 225, 215, br); case 3: return RGB(5, 5, 5); case 4: return shc(40, 25, 20, br); default: return shc(150, 20, 20, br); } }
void hsRoundSetup() {
  hsHid = false; hsHidSp = -1; hsMsgT = 0; hsVisited = 0; hsAI = 0; hsWin = false; hsBreath = 1; hsPh = 0; hsLostT = 0; hsBfsT = 0; hsRel = 14;
  hsRole = hsRound & 1; hsT = hsRole == 0 ? 12.0f : 10.0f; hsa = 0;
  if (hsRole == 0) {
    int c, r; do { c = random(3); r = random(3); } while (c == 1 && r == 1); hsx = 5 * c + 3.5f; hsy = 5 * r + 3.5f; wkSet(hsK, 8.5f, 8.5f);
  } else { hsx = 8.5f; hsy = 8.5f; hsHidSp = random(hsN); }
}
void hsInit() {
  if (!hsLut) { hsLut = true; for (int b = 0; b < 256; b++) { hsCL[b] = shc(205, 200, 190, b); hsFL[b] = shc(120, 82, 52, b); } }
  hsBuild(); lives = 3; hsRound = 0; hsRoundSetup(); setCone(0);
}
void hsPickPatrol() {
  int16_t d2[HSW * HSW]; bfsG(hsm, HSW, HSW, (int)hsK.x, (int)hsK.y, d2);
  int best = -1, bs = 99999;
  for (int i = 0; i < hsN; i++) { if (hsVisited & (1u << i)) continue; int s = d2[hsS[i].fy * HSW + hsS[i].fx] * 4 + random(0, 10); if (s < bs) { bs = s; best = i; } }
  if (best < 0) { hsVisited = 0; best = random(hsN); }
  hsGoal = best; bfsG(hsm, HSW, HSW, hsS[best].fx, hsS[best].fy, hsd);
}
void hsEnd(bool win, const char *m) {
  hsPh = 2; hsT = 2.8f; hsMsg = m; hsWin = win;
  if (win) { score += 100 + (int)hsRel; SFX(SFX_WIN); } else { lives--; shake = 0.3f; SFX(SFX_SCARE); }
}
void hsUpd(float dt) {
  noPause = true;
  if (hsMsgT > 0) hsMsgT -= dt;
  if (hsPh == 2) { hsT -= dt; if (hsT <= 0) { if (lives <= 0) die(); else { hsRound++; hsRoundSetup(); } } return; }
  hsT -= dt;
  if (hsPh == 0) { if (hsT <= 0) { hsPh = 1; hsT = hsRole == 0 ? 45.0f : fmaxf(40.0f, 70.0f - hsRound * 2); hsRel = hsT; if (hsRole == 0) { hsAI = 1; hsPickPatrol(); } } }
  else if (hsT <= 0) { if (hsRole == 0) hsEnd(true, "SURVIVED!"); else hsEnd(false, "TIME UP"); return; }
  hsRel = hsT;
  // حركة اللاعب
  float mv = (held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0); hsSprint = false;
  if (hsHid) {
    float da = ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 1.2f * dt; float na = hsa + da; if (fabsf(na - hsHidA) < 0.7f) hsa = na;
    if (held[K_B] && hsBreath > 0) hsBreath -= dt * 0.3f; else hsBreath = fminf(1, hsBreath + dt * 0.18f);
    if (hsBreath < 0) hsBreath = 0;
  } else {
    hsa += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 2.4f * sensM() * dt;
    hsSprint = held[K_B] && mv > 0; float sp = hsSprint ? 3.2f : 2.0f; if (mv < 0) sp *= 0.6f;
    movePl(hsm, HSW, HSW, hsx, hsy, cosf(hsa) * mv * sp * dt, sinf(hsa) * mv * sp * dt, 0.2f);
    if (mv != 0) hsBob = sinf(millis() * (hsSprint ? 0.018f : 0.011f)) * (hsSprint ? 2.0f : 1.0f); else hsBob *= 0.9f;
  }
  // تفاعل A
  if (prs[K_A]) {
    if (hsHid) { hsHid = false; hsx = hsS[hsHidSp].fx + 0.5f; hsy = hsS[hsHidSp].fy + 0.5f; SFX(SFX_BACK); }
    else {
      int bi = -1; float bd = 9;
      for (int i = 0; i < hsN; i++) {
        float dx = hsS[i].x + 0.5f - hsx, dy = hsS[i].y + 0.5f - hsy, d = sqrtf(dx * dx + dy * dy), an = atan2f(dy, dx) - hsa; while (an > PI) an -= 2 * PI; while (an < -PI) an += 2 * PI;
        if (d < 1.4f && fabsf(an) < 0.9f && d < bd) { bd = d; bi = i; }
      }
      if (bi >= 0) {
        if (hsRole == 0) { hsHid = true; hsHidSp = bi; hsHidA = atan2f(hsS[bi].fy - hsS[bi].y, hsS[bi].fx - hsS[bi].x); hsa = hsHidA; hsx = hsS[bi].x + 0.5f; hsy = hsS[bi].y + 0.5f; SFX(SFX_OK); hsMsg = "HIDING (HOLD B)"; hsMsgT = 2; }
        else if (hsPh == 1) {
          if (bi == hsHidSp) { hsEnd(true, "FOUND HER!"); return; }
          hsMsg = "EMPTY..."; hsMsgT = 1.2f; SFX(SFX_CREAK);
        }
      }
    }
  }
  if (hsRole == 0 && hsPh == 1) {   // ذكاء الباحث
    float dx = hsx - hsK.x, dy = hsy - hsK.y, d = sqrtf(dx * dx + dy * dy);
    bool los = losG(hsm, HSW, HSW, hsK.x, hsK.y, hsx, hsy), see = !hsHid && los && d < 7.0f, hear = !hsHid && d < 4.0f && hsSprint;
    if ((see || hear) && hsAI != 3) { hsAI = 3; hsLostT = 0; SFX(SFX_CREAK); }
    if (hsAI == 3) {
      if (see || hear) hsLostT = 0; else hsLostT += dt;
      if (hsHid || hsLostT > 4.5f) { hsAI = 1; hsPickPatrol(); }
      else if (los && d < 2.2f && d > 0.01f) { float st = (2.2f + 0.1f * hsRound) * dt; hsK.x += dx / d * st; hsK.y += dy / d * st; hsK.cx = hsK.tx = (int)hsK.x; hsK.cy = hsK.ty = (int)hsK.y; }
      else { hsBfsT -= dt; if (hsBfsT <= 0) { hsBfsT = 0.3f; bfsG(hsm, HSW, HSW, (int)hsx, (int)hsy, hsd); } wkStep(hsK, hsm, HSW, HSW, hsd, 2.0f + 0.1f * hsRound, dt); }
    } else if (hsAI == 1) { if (wkStep(hsK, hsm, HSW, HSW, hsd, 1.15f + 0.08f * hsRound, dt)) { hsAI = 2; hsChk = 1.2f; } }
    else if (hsAI == 2) {
      hsChk -= dt;
      if (hsChk <= 0) {
        hsVisited |= 1u << hsGoal; SFXL(SFX_CREAK);
        if (hsHid && hsHidSp == hsGoal) { float pf = (held[K_B] && hsBreath > 0) ? 0.12f : 0.7f; if (random(1000) < pf * 1000) { hsEnd(false, "FOUND YOU!"); return; } hsMsg = "CLOSE CALL..."; hsMsgT = 1.5f; }
        hsAI = 1; hsPickPatrol();
      }
    }
    if (!hsHid && d < 0.65f) { hsEnd(false, "CAUGHT!"); return; }
    hsHeartT -= dt; if (hsHeartT <= 0 && d < 5) { hsHeartT = 0.35f + d * 0.12f; SFXL(SFX_HEART); }
  }
  if (hsRole == 1 && hsPh == 1) {   // المختبئ بيغير مكانه أحيانًا
    hsRel -= 0; static float rl = 12; rl -= dt;
    if (rl <= 0) { rl = 14 + random(8); float dx = hsS[hsHidSp].x + 0.5f - hsx, dy = hsS[hsHidSp].y + 0.5f - hsy; if (sqrtf(dx * dx + dy * dy) > 5.0f) { hsHidSp = random(hsN); sndL(300, 40); } }
  }
  snprintf(oledX, 24, "R%d %s %ds", hsRound + 1, hsRole == 0 ? "HIDE" : "SEEK", (int)hsT);
}
void hsDraw() {
  FpsV v = {hsm, HSW, HSW, hsx, hsy, hsa, 64 + (int)hsBob, 175, 150, 0.05f, 1.0f, hsWall, hsCL, hsFL, 0, 0, false};
  setCone(0); fpsRender(v);
  if (hsRole == 0 && hsPh >= 1 && !(hsHid && false)) drawSpr(v, hsK.x, hsK.y, 0.8f, 0.4f, dollShape, dollCol);
  if (hsRole == 1 && hsPh == 2 && hsWin) drawSpr(v, hsS[hsHidSp].fx + 0.5f, hsS[hsHidSp].fy + 0.5f, 0.8f, 0.4f, dollShape, dollCol);
  if (hsHid) {
    for (int y = 0; y < SH; y++) if (y < 50 || y > 78) hl(0, SW - 1, y, C_K);
    for (int y = 50; y <= 78; y++) for (int x = 0; x < SW; x += 9) { hl(x, x + 1, y, C_K); }
    for (int y = 50; y <= 78; y++) { int e = (y < 54 || y > 74) ? 12 : 4; hl(0, e, y, C_K); hl(SW - 1 - e, SW - 1, y, C_K); }
    txtC("B: HOLD BREATH", 94, RGB(160, 160, 190)); cv.drawRect(34, 106, 60, 5, RGB(120, 120, 150)); cv.fillRect(35, 107, (int)(58 * hsBreath), 3, RGB(120, 220, 255));
  }
  char b[28];
  if (hsPh == 0) {
    if (hsRole == 1) { for (int i = 0; i < SW * SH; i++) FB[i] = (FB[i] >> 2) & 0x39E7; snprintf(b, 28, "%d", (int)hsT + 1); txtC(b, 40, C_W, 4); txtC("COUNTING... SHE HIDES", 90, RGB(255, 220, 120)); }
    else { snprintf(b, 28, "HIDE! %d", (int)hsT + 1); txtS(b, 4, 4, RGB(255, 220, 120), 2); }
  } else if (hsPh == 1) {
    snprintf(b, 28, "%s %d", hsRole == 0 ? "SURVIVE" : "FIND HER", (int)hsT); txtS(b, 4, 3, hsRole == 0 ? RGB(255, 160, 100) : RGB(120, 255, 160));
    if (hsRole == 1) {
      float dx = hsS[hsHidSp].x + 0.5f - hsx, dy = hsS[hsHidSp].y + 0.5f - hsy, d = sqrtf(dx * dx + dy * dy);
      txtS(d < 3 ? "HOT!" : (d < 6 ? "WARM" : "COLD"), 4, 114, d < 3 ? RGB(255, 80, 60) : (d < 6 ? RGB(255, 200, 60) : RGB(100, 160, 255)));
      txtS("A:OPEN  B:RUN", 56, 114, RGB(200, 200, 220));
    } else if (!hsHid) txtS("A:HIDE  B:RUN", 56, 114, RGB(200, 200, 220));
  }
  for (int i = 0; i < lives; i++) heart(SW - 10 - i * 9, 14, RGB(255, 60, 90));
  if (hsMsgT > 0) txtS(hsMsg, (SW - (int)strlen(hsMsg) * 6) / 2, 30, RGB(255, 255, 160));
  if (hsPh == 2) { dim(); txtC(hsMsg, 54, hsWin ? RGB(120, 255, 160) : RGB(255, 90, 90), 2 - ((int)strlen(hsMsg) > 9 ? 1 : 0)); }
}

// ---------- 13) ANOMALY (رعب: ممر مترو - لو شفت حاجة غلط ارجع، لو مفيش كمل. 8 مرات صح = تهرب) ----------
#define AN_W 18
#define AN_H 4
uint8_t anm[AN_W * AN_H];
float anpx, anpy, ana, anBob, anFlick = 1, anMsgT, anFigX, anFigT, anCrawlX, anCrawlT, anScareT, anPostT, anFlickT;
int anLevel, anKind, anPrevKind, anPh, anSavedLv, anScareVar; bool anScared, anDie; const char *anMsg = "";
uint16_t anCL[256], anFL[256]; bool anLut = false;
const uint16_t DIG[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
uint16_t anWall(int mx, int my, int side, float u, float v, int br) {
  int bb = (int)(br * (0.78f + 0.22f * cosf((mx + u) * 1.5708f)) * anFlick); if (bb > 255) bb = 255;
  bool red = anKind == 5;
  if (mx == 0) { if (v < 0.25f && u > 0.25f && u < 0.75f) return RGB(40, 220, 90); return shc(110, 115, 120, bb); }
  if (mx == AN_W - 1) { if (v > 0.12f && v < 0.4f && u > 0.3f && u < 0.7f) return shc(210, 210, 170, bb); return shc(110, 115, 120, bb); }
  int r, g, b;
  if (v > 0.62f) { float fu = u * 4 - floorf(u * 4), fv = (v - 0.62f) * 10 - floorf((v - 0.62f) * 10); if (fu < 0.07f || fv < 0.1f) { r = 90; g = 96; b = 92; } else { r = 150; g = 162; b = 152; } }
  else { r = 196; g = 196; b = 180; if (v > 0.6f) { r = 120; g = 120; b = 110; } }
  bool poster = (my == 0 && (mx == 4 || mx == 13)) || (my == 3 && mx == 9);
  if (poster && u > 0.18f && u < 0.82f && v > 0.14f && v < 0.56f) {
    bool horror = (anKind == 8) || (anKind == 1 && my == 0 && mx == 4);
    int var = horror ? ((mx & 1) ? 3 : 4) : ((mx * 3 + my) % 3);
    float fu = (u - 0.18f) / 0.64f, fv = (v - 0.14f) / 0.42f;
    if (fu < 0.06f || fu > 0.94f || fv < 0.06f || fv > 0.94f) return shc(60, 40, 30, bb);
    return faceCol((fu - 0.06f) / 0.88f, (fv - 0.06f) / 0.88f, var, bb, mx);
  }
  if (my == 0 && mx == 2 && u > 0.3f && u < 0.7f && v > 0.16f && v < 0.46f) {
    float fu = (u - 0.3f) / 0.4f, fv = (v - 0.16f) / 0.3f; int col = (int)(fu * 3), rw = (int)(fv * 5);
    if (col > 2) col = 2; if (rw > 4) rw = 4;
    return ((DIG[constrain(anLevel, 0, 9)] >> (14 - (rw * 3 + col))) & 1) ? RGB(255, 255, 255) : RGB(10, 70, 30);
  }
  if (anKind == 2 && my == 3 && mx >= 10 && mx <= 12) {
    int ci = (int)(u * 4), cj = (int)(v * 5); uint32_t h = hh(ci * 17 + cj * 29 + mx * 7);
    if (h % 3 == 0) { float ex = (ci + 0.5f) / 4, ey = (cj + 0.5f) / 5; if (inEll(u, v, ex, ey, 0.09f, 0.05f)) return inEll(u, v, ex + ((h >> 3) & 1 ? 0.015f : -0.015f), ey, 0.025f, 0.04f) ? RGB(210, 0, 0) : shc(235, 235, 225, bb); }
    r = r * 5 / 10; g = g * 4 / 10; b = b * 4 / 10;
  }
  if (anKind == 3 && my == 0 && mx >= 10 && mx <= 12) {
    int s = (int)(u * 20); uint32_t h = hh(s * 13 + mx * 5);
    if (v < 0.15f + (h % 50) / 100.0f && (h & 3) != 0) { r = 150; g = 0; b = 0; }
    if (inEll(u, v, 0.5f, 0.4f, 0.12f, 0.16f)) { r = 140; g = 0; b = 0; }
  }
  if (anKind == 6 && my == 3 && mx == 7 && u > 0.2f && u < 0.8f && v > 0.1f) {
    if (u > 0.28f && u < 0.72f && v > 0.16f) return (inEll(u, v, 0.42f, 0.45f, 0.04f, 0.025f) || inEll(u, v, 0.58f, 0.45f, 0.04f, 0.025f)) ? RGB(255, 255, 230) : RGB(2, 0, 0);
    return shc(90, 60, 35, bb);
  }
  if (red) { g = g * 3 / 10; b = b * 3 / 10; }
  return shc(r, g, b, bb);
}
uint8_t slendShape(float u, float v) {
  float ad = fabsf(u - 0.5f);
  if (inEll(u, v, 0.5f, 0.07f, 0.09f, 0.06f)) return 2;
  if (v > 0.12f && v < 0.17f && ad < 0.03f) return 2;
  if (v >= 0.15f && v < 0.56f) { if (ad < 0.13f - (v - 0.15f) * 0.05f) { if (ad < 0.012f && v > 0.2f && v < 0.45f) return 3; return 1; } }
  if (v >= 0.17f && v < 0.75f && fabsf(ad - (0.15f + (v - 0.17f) * 0.07f)) < 0.022f) return 1;
  if (v >= 0.56f && fabsf(ad - 0.05f) < 0.04f) return 1;
  return 0;
}
uint16_t slendCol(uint8_t k, int br) { return k == 1 ? shc(10, 10, 14, br < 60 ? 60 : br) : (k == 2 ? shc(238, 238, 232, br < 90 ? 90 : br) : RGB(190, 10, 10)); }
uint8_t crawlShape(float u, float v) {
  if (inEll(u, v, 0.5f, 0.42f, 0.13f, 0.14f)) { if (inEll(u, v, 0.45f, 0.4f, 0.025f, 0.03f) || inEll(u, v, 0.55f, 0.4f, 0.025f, 0.03f)) return 3; if (inEll(u, v, 0.5f, 0.5f, 0.06f, 0.03f)) return 3; return 1; }
  if (inEll(u, v, 0.5f, 0.78f, 0.2f, 0.16f)) return 1;
  if (v > 0.55f && (fabsf(u - (0.3f - (v - 0.55f) * 0.5f)) < 0.03f || fabsf(u - (0.7f + (v - 0.55f) * 0.5f)) < 0.03f)) return 1;
  return 0;
}
uint16_t crawlCol(uint8_t k, int br) { return k == 1 ? shc(205, 195, 185, br < 80 ? 80 : br) : RGB(5, 0, 0); }
void anNew() {
  anpx = 2.5f; anpy = 1.6f; ana = 0; anKind = 0;
  if (random(100) >= 34) { do anKind = 1 + random(8); while (anKind == anPrevKind); }
  anPrevKind = anKind; anFigX = 15.0f; anFigT = 0; anCrawlX = 11.0f; anCrawlT = 0; anPostT = 0; anScared = false;
}
bool anLoad(AnSave &s) { return pref.getBytes("asv", &s, sizeof(s)) == sizeof(s) && s.magic == 0x414E0001; }
void anSave() { if (anPh != 2) return; AnSave s = {0x414E0001, anLevel, score}; pref.putBytes("asv", &s, sizeof(s)); }
void anInit() {
  if (!anLut) { anLut = true; for (int b = 0; b < 256; b++) { anCL[b] = shc(70, 72, 70, b); anFL[b] = shc(92, 94, 88, b); } }
  memset(anm, 1, sizeof(anm)); for (int y = 1; y <= 2; y++) for (int x = 1; x < AN_W - 1; x++) anm[y * AN_W + x] = 0;
  lives = 3; anLevel = 0; anPrevKind = 0; anScareT = 0; anDie = false; anMsgT = 0; anBob = 0; anNew(); anSavedLv = 0;
  AnSave s; if (anLoad(s) && s.lvl > 0) anPh = 0; else { anPh = 1; storyStart(1); }
}
void anJudge(bool forward) {
  bool ok = forward ? (anKind == 0) : (anKind != 0);
  if (ok) {
    anLevel++; score += 100; SFX(SFX_POWER); anMsg = "CORRECT"; anMsgT = 1.5f;
    if (anLevel >= 8) { won = true; score += 1000; pref.remove("asv"); die(); return; }
    anSave(); anNew();
  } else {
    anLevel = 0; lives--; anMsg = "WRONG. START OVER"; anMsgT = 2; anScareVar = 3 + random(2); anScareT = 1.3f; SFX(SFX_SCARE); shake = 0.4f;
    anSave(); if (lives <= 0) anDie = true; anNew();
  }
}
void anUpd(float dt) {
  if (anPh == 0) {
    noPause = true; AnSave s;
    if (prs[K_A] || prs[K_ST]) { if (anLoad(s)) { anLevel = s.lvl; score = s.score; } anPh = 2; SFX(SFX_OK); }
    else if (prs[K_B]) { pref.remove("asv"); anPh = 1; storyStart(1); }
    return;
  }
  if (anPh == 1) { if (storyUpd(dt)) { anPh = 2; anNew(); anFlick = 1; } return; }
  if (anMsgT > 0) anMsgT -= dt;
  if (anScareT > 0) { anScareT -= dt; if (anScareT <= 0 && anDie) die(); return; }
  anFlickT -= dt;
  if (anFlickT <= 0) { anFlickT = 0.05f + random(150) / 1000.0f; anFlick = (anKind == 5 || anKind == 4 || anKind == 7) ? ((random(100) < 25) ? 0.35f : 1.0f) : ((random(100) < 3) ? 0.7f : 1.0f); }
  ana += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 2.3f * sensM() * dt;
  float mv = (held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0), sp = held[K_B] ? 2.7f : 1.7f; if (mv < 0) sp *= 0.6f;
  movePl(anm, AN_W, AN_H, anpx, anpy, cosf(ana) * mv * sp * dt, sinf(ana) * mv * sp * dt, 0.2f);
  if (mv != 0) anBob = sinf(millis() * 0.011f) * 1.2f; else anBob *= 0.9f;
  float ang = ana; while (ang > PI) ang -= 2 * PI; while (ang < -PI) ang += 2 * PI;
  // الشخصية الواقفة (تقرب لما متبصش عليها)
  if (anKind == 4) {
    bool look = fabsf(ang) < 0.7f && anFigX > anpx; if (!look) anFigT += dt; if (anFigT > 2.4f) { anFigT = 0; anFigX = fmaxf(anpx + 1.3f, anFigX - 0.8f); sndL(150, 80); }
    if (fabsf(anFigX - anpx) < 1.4f && !anScared) { anScared = true; anScareVar = 4; anScareT = 1.3f; SFX(SFX_SCARE); lives--; shake = 0.4f; if (lives <= 0) anDie = true; anNew(); return; }
  }
  if (anKind == 7) {
    anCrawlT += dt; if (anCrawlT > 1.4f) { anCrawlT = 0; anCrawlX = fmaxf(anpx + 1.2f, anCrawlX - 0.9f); sndL(120, 60); }
    if (fabsf(anCrawlX - anpx) < 1.3f && !anScared) { anScared = true; anScareVar = 3; anScareT = 1.3f; SFX(SFX_SCARE); lives--; shake = 0.4f; if (lives <= 0) anDie = true; anNew(); return; }
  }
  if (anKind == 1 || anKind == 8) {
    bool nr = (anKind == 1) ? fabsf(anpx - 4.5f) < 1.6f : (fabsf(anpx - 4.5f) < 1.6f || fabsf(anpx - 9.5f) < 1.6f || fabsf(anpx - 13.5f) < 1.6f);
    if (nr) anPostT += dt; else anPostT = fmaxf(0, anPostT - dt);
    if (anPostT > 1.8f && !anScared) { anScared = true; anScareVar = 3; anScareT = 1.0f; SFX(SFX_SCARE); shake = 0.3f; }
  }
  if (anKind != 0 && ((millis() / 3000) & 1) && random(100) < 2) SFXL(SFX_CREAK);
  if (anpx > 16.6f) anJudge(true); else if (anpx < 1.4f) anJudge(false);
  snprintf(oledX, 24, "EXIT %d / 8", anLevel);
}
void anDraw() {
  if (anPh == 0) { AnSave s; char b[24] = ""; if (anLoad(s)) snprintf(b, 24, "EXIT %d  SC %d", s.lvl, s.score); svMenuDraw("ANOMALY", b); return; }
  if (anPh == 1) { storyDraw(); return; }
  if (anScareT > 0) { scareDraw(anScareVar, 1.3f - anScareT); return; }
  FpsV v = {anm, AN_W, AN_H, anpx, anpy, ana, 64 + (int)anBob, 190, 160, 0.012f, 1.0f, anWall, anCL, anFL, 0, 0, false};
  setCone(0); fpsRender(v);
  if (anKind == 4) drawSpr(v, anFigX, 1.6f, 1.55f, 0.42f, slendShape, slendCol);
  if (anKind == 7) drawSpr(v, anCrawlX, 1.6f, 0.45f, 0.7f, crawlShape, crawlCol);
  fxNoise(anKind ? 120 : 25);
  if (anKind == 5 || anKind == 4 || anKind == 7) fxVig(0.5f);
  if (anKind == 2 || anKind == 3) fxGlitch(1);
  char b[24]; snprintf(b, 24, "EXIT %d/8", anLevel); txtS(b, 3, 3, C_W);
  for (int i = 0; i < lives; i++) heart(SW - 10 - i * 9, 2, RGB(255, 60, 90));
  txtS("BACK=BAD  GO=OK", 20, 118, RGB(150, 150, 170));
  if (anMsgT > 0) txtC(anMsg, 56, ((anMsg[0] == 'C') ? RGB(120, 255, 160) : RGB(255, 90, 90)), 1);
}

// ---------- 14) LOST ROAD (رعب بقصة: العربية اتقلبت في الغابة - دور على 5 قطع غيار - سلندر مان بيطاردك) ----------
#define FW 40
uint8_t fom[FW * FW]; int16_t fod[FW * FW];
struct FItem { float x, y; uint8_t k; bool on; };
FItem fit[9];
float fox, foy, foa, fBat, fStam, fBob, fWalk, fFear, fFlick, fFlickT, fHeartT, fMsgT, fScareT, fRep, fHideT, fSaveT, slBfsT, slTimer, slStare, slStep, slExpo, slLostT, fT, wrx, wry, slx, sly, fCreakT;
bool fTorch, fMoving, fSprint, fCaught, fHide;
int foPh, fParts, slMode; uint32_t foSeed; Wk slK; const char *fMsg = "";
uint16_t foCL[256], foFL[256]; bool foLut = false;
bool foLoad(FoSave &s) { return pref.getBytes("fsv", &s, sizeof(s)) == sizeof(s) && s.magic == 0x464F0001; }
void foSave() {
  if (foPh != 2) return; FoSave s = {0x464F0001, foSeed, fox, foy, foa, fBat, score, 0};
  for (int i = 0; i < 9; i++) if (!fit[i].on) s.mask |= (1 << i);
  pref.putBytes("fsv", &s, sizeof(s));
}
void foGen(uint32_t sd) {
  foSeed = sd; randomSeed(sd); memset(fom, 1, sizeof(fom));
  for (int y = 1; y < FW - 1; y++) for (int x = 1; x < FW - 1; x++) { float n = vnz(x, y, 6, sd); fom[y * FW + x] = (hh(x * 73 + y * 131 + sd) % 100 < 17 || n > 0.72f) ? 1 : 0; }
  for (int y = 17; y <= 23; y++) for (int x = 1; x <= 7; x++) fom[y * FW + x] = 0;
  wrx = 4.5f; wry = 20.5f; bfsG(fom, FW, FW, 3, 21, fod);
  int k = 0;
  for (int t = 0; t < 4000 && k < 9; t++) {
    int x = random(2, FW - 2), y = random(2, FW - 2); if (fom[y * FW + x] || fod[y * FW + x] >= 999) continue;
    int mind = (k < 5) ? 16 : 6; if (fod[y * FW + x] < mind) continue;
    bool ok = true; for (int j = 0; j < k; j++) if (fabsf(fit[j].x - x - 0.5f) + fabsf(fit[j].y - y - 0.5f) < 4) ok = false; if (!ok) continue;
    fit[k].x = x + 0.5f; fit[k].y = y + 0.5f; fit[k].k = k < 5 ? 0 : 1; fit[k].on = true; k++;
  }
  for (; k < 9; k++) { fit[k].x = 10.5f + k; fit[k].y = 20.5f; fit[k].k = k < 5 ? 0 : 1; fit[k].on = true; fom[20 * FW + 10 + k] = 0; }
  randomSeed(micros());
}
void foReset() {
  fBat = 1; fStam = 1; fTorch = true; fBob = 0; fWalk = 0; fFear = 0; fFlick = 1; fFlickT = 0; fHeartT = 1; fMsgT = 3; fMsg = "FIND 5 CAR PARTS"; fScareT = 0; fRep = 0; fHideT = 0;
  fCaught = false; fSaveT = 0; slMode = 0; slTimer = 25; slStare = 0; slExpo = 0; slBfsT = 0; fCreakT = 12; fT = 0; fParts = 0;
}
void foNew() { foGen(micros() ^ (uint32_t)random(100000)); foReset(); fox = 3.5f; foy = 21.5f; foa = 0; foPh = 2; }
void foInit() {
  if (!foLut) { foLut = true; for (int b = 0; b < 256; b++) { foCL[b] = shc(34, 48, 86, b); foFL[b] = shc(52, 74, 40, b); } }
  lives = 0; foGen(1); foReset(); FoSave s;
  if (foLoad(s)) foPh = 0; else { foPh = 1; storyStart(2); }
}
uint16_t foWall(int mx, int my, int side, float u, float v, int br) {
  uint32_t h = hh(mx * 31 + my * 57); int s = (int)(u * 16);
  if (v < 0.06f) return shc(8, 22, 14, br);
  if (h % 7 == 0) { int g = ((((int)(v * 14 + h)) % 5 == 0) && ((s & 3) < 2)) ? 30 : (200 + ((s * 7) & 15)); return shc(g, g, g - 8, br); }
  int n = ((s * 37 + h) & 7), r = 70 + n * 4, g = 48 + n * 3, b = 32 + n * 2;
  if (v > 0.9f) { r = 40; g = 70; b = 36; }
  if (((int)(v * 30) + s * 5 + h) % 17 == 0) { r -= 25; g -= 18; b -= 12; }
  return shc(r, g, b, br);
}
uint8_t wreckShape(float u, float v) {
  if (v > 0.15f && v < 0.35f && u > 0.2f && u < 0.8f) return 2;
  if (v >= 0.35f && v < 0.8f && u > 0.05f && u < 0.95f) return 1;
  if (v >= 0.8f && ((u > 0.15f && u < 0.32f) || (u > 0.68f && u < 0.85f))) return 3;
  return 0;
}
uint16_t wreckCol(uint8_t k, int br) { return k == 1 ? shc(150, 40, 40, br) : (k == 2 ? shc(90, 25, 25, br) : shc(25, 25, 28, br)); }
void foSpawnSl() {
  for (int t = 0; t < 60; t++) {
    int x = random(2, FW - 2), y = random(2, FW - 2); if (fom[y * FW + x]) continue;
    float dx = x + 0.5f - fox, dy = y + 0.5f - foy, d = sqrtf(dx * dx + dy * dy); if (d < 9 || d > 14) continue;
    slx = x + 0.5f; sly = y + 0.5f; slMode = 1; slStare = 0; slStep = 3.2f; slExpo = 0; wkSet(slK, slx, sly); SFXL(SFX_CREAK); return;
  }
  slTimer = 5;
}
void foMonster(float dt) {
  float dx = fox - slx, dy = foy - sly, d = sqrtf(dx * dx + dy * dy);
  if (slMode == 0) { slTimer -= dt; if (slTimer <= 0) foSpawnSl(); return; }
  bool los = losG(fom, FW, FW, slx, sly, fox, foy);
  float at = atan2f(sly - foy, slx - fox) - foa; while (at > PI) at -= 2 * PI; while (at < -PI) at += 2 * PI;
  bool looking = los && fabsf(at) < 0.55f && d < 12;
  if (slMode == 1) {
    if (looking) { slStare += dt; if (slStare > 3.5f && d < 9) { slMode = 2; SFX(SFX_GROWL); } } else slStare = fmaxf(0, slStare - dt);
    if (!looking) { slStep -= dt; if (slStep <= 0) { slStep = fmaxf(1.4f, 3.2f - 0.3f * fParts); for (int t = 0; t < 12; t++) { float a = random(628) / 100.0f, r = fmaxf(2.0f, d - 2.4f); int cx = (int)(fox + cosf(a) * r), cy = (int)(foy + sinf(a) * r); if (cx > 0 && cy > 0 && cx < FW && cy < FW && !fom[cy * FW + cx]) { slx = cx + 0.5f; sly = cy + 0.5f; SFXL(SFX_CREAK); break; } } } }
    if (fSprint && d < 8) slMode = 2;
    if (fTorch && los && d < 7) { slExpo += dt; if (slExpo > 2.0f) slMode = 2; } else slExpo = fmaxf(0, slExpo - dt);
    if (d < 2.2f) slMode = 2;
    if (fHide && d > 1.8f) { slMode = 0; slTimer = 14 + random(8); SFXL(SFX_CREAK); }
    if (slMode == 2) { wkSet(slK, slx, sly); slLostT = 0; }
  } else {
    float spd = fminf(2.6f, 1.35f + 0.08f * fParts);
    if (los && d < 2.5f && d > 0.01f) { float st = spd * dt; slK.x += dx / d * st; slK.y += dy / d * st; slK.cx = slK.tx = (int)slK.x; slK.cy = slK.ty = (int)slK.y; }
    else { slBfsT -= dt; if (slBfsT <= 0) { slBfsT = 0.35f; bfsG(fom, FW, FW, (int)fox, (int)foy, fod); } wkStep(slK, fom, FW, FW, fod, spd, dt); }
    slx = slK.x; sly = slK.y;
    if (fHide && d > 1.8f) { slMode = 0; slTimer = 16 + random(8); fMsg = "HE LOST YOU"; fMsgT = 2.5f; SFX(SFX_CREAK); }
    if (d > 18) { slLostT += dt; if (slLostT > 6) { slMode = 0; slTimer = 12; } } else slLostT = 0;
    if (d < 0.65f && !fCaught) { fCaught = true; fScareT = 1.6f; SFX(SFX_SCARE); shake = 0.6f; }
  }
}
void foUpd(float dt) {
  if (foPh == 0) {
    noPause = true; FoSave s;
    if (prs[K_A] || prs[K_ST]) {
      if (foLoad(s)) { foGen(s.seed); foReset(); fox = s.x; foy = s.y; foa = s.a; fBat = s.bat; score = s.score; fParts = 0; for (int i = 0; i < 9; i++) if (s.mask & (1 << i)) { fit[i].on = false; if (i < 5) fParts++; } }
      foPh = 2; SFX(SFX_OK);
    } else if (prs[K_B]) { pref.remove("fsv"); foPh = 1; storyStart(2); }
    return;
  }
  if (foPh == 1) { if (storyUpd(dt)) foNew(); return; }
  if (fCaught) { fScareT -= dt; if (fScareT <= 0) die(); return; }
  fT += dt; if (fMsgT > 0) fMsgT -= dt; fSaveT += dt;
  foa += ((held[K_RT] ? 1 : 0) - (held[K_LF] ? 1 : 0)) * 2.3f * sensM() * dt;
  float mv = (held[K_UP] ? 1 : 0) - (held[K_DN] ? 1 : 0); fMoving = mv != 0; fSprint = held[K_A] && mv > 0 && fStam > 0.05f;
  float sp = fSprint ? 3.0f : 1.8f; if (mv < 0) sp *= 0.6f;
  if (fSprint) fStam -= dt * 0.25f; else fStam = fminf(1, fStam + dt * (fMoving ? 0.12f : 0.25f)); if (fStam < 0) fStam = 0;
  movePl(fom, FW, FW, fox, foy, cosf(foa) * mv * sp * dt, sinf(foa) * mv * sp * dt, 0.2f);
  if (fMoving) { fWalk += dt * (fSprint ? 12 : 7); fBob = sinf(fWalk) * (fSprint ? 2.0f : 1.0f); } else fBob *= 0.9f;
  if (prs[K_B] && (fBat > 0.01f || fTorch)) { fTorch = !fTorch; sndL(fTorch ? 1100 : 700, 24); }
  if (fTorch) { fBat -= dt / 150.0f; if (fBat <= 0) { fBat = 0; fTorch = false; fMsg = "TORCH DEAD"; fMsgT = 2; } }
  if (!fMoving && !fTorch) fHideT += dt; else fHideT = 0;
  fHide = fHideT >= 3.5f;
  fFlickT -= dt; if (fFlickT <= 0) { fFlickT = 0.05f + random(120) / 1000.0f; float p = fFear * 0.3f + (fBat < 0.2f ? 0.3f : 0.01f); fFlick = (random(1000) / 1000.0f < p) ? 0.2f + random(300) / 1000.0f : 1.0f; }
  for (int i = 0; i < 9; i++) if (fit[i].on) {
    float dx = fit[i].x - fox, dy = fit[i].y - foy;
    if (dx * dx + dy * dy < 0.4f) {
      fit[i].on = false; SFX(SFX_POWER);
      if (fit[i].k == 0) { fParts++; score += 100; fMsg = fParts >= 5 ? "ALL PARTS! GO TO CAR" : "CAR PART FOUND"; fMsgT = 2.5f; }
      else { fBat = fminf(1, fBat + 0.6f); fMsg = "BATTERY"; fMsgT = 1.5f; }
      foSave();
    }
  }
  float wd = sqrtf((wrx - fox) * (wrx - fox) + (wry - foy) * (wry - foy));
  if (fParts >= 5 && wd < 1.9f) {
    if (held[K_A]) { fRep += dt; if (fRep >= 3.0f) { won = true; score += 1000; pref.remove("fsv"); die(); return; } } else fRep = fmaxf(0, fRep - dt);
    fMsg = "HOLD A: REPAIR CAR"; fMsgT = 0.3f;
  } else fRep = 0;
  foMonster(dt);
  float f = 0; if (slMode) { float d = sqrtf((fox - slx) * (fox - slx) + (foy - sly) * (foy - sly)); f = 1.0f - d / 10.0f; if (f < 0) f = 0; if (slStare > 0) f = fmaxf(f, 0.25f + slStare * 0.1f); if (slMode == 2) f = fmaxf(f, 0.4f); }
  fFear += (f - fFear) * fminf(1, dt * 3);
  fHeartT -= dt; if (fHeartT <= 0) { fHeartT = 60.0f / (55.0f + fFear * 110.0f); if (fFear > 0.12f) SFXL(SFX_HEART); }
  fCreakT -= dt; if (fCreakT <= 0) { fCreakT = 12 + random(150) / 10.0f; SFXL(SFX_CREAK); }
  if (fSaveT > 30) { fSaveT = 0; foSave(); }
  snprintf(oledX, 24, "PARTS %d/5 %s", fParts, slMode == 2 ? "RUN!" : (slMode == 1 ? "WATCHED" : "..."));
}
void foDraw() {
  if (foPh == 0) { FoSave s; char b[24] = ""; if (foLoad(s)) { int c = 0; for (int i = 0; i < 5; i++) if (s.mask & (1 << i)) c++; snprintf(b, 24, "PARTS %d/5  SC %d", c, s.score); } svMenuDraw("LOST ROAD", b); return; }
  if (foPh == 1) { storyDraw(); return; }
  if (fCaught) { scareDraw(3, 1.6f - fScareT); return; }
  FpsV v = {fom, FW, FW, fox, foy, foa, 64 + (int)fBob, 105, 88, 0.045f, 2.4f, foWall, foCL, foFL, RGB(20, 30, 44), 15, false};
  float tp = fTorch ? fFlick * (0.35f + 0.65f * fminf(1.0f, fBat * 3.0f)) : 0.0f; setCone(tp);
  fpsRender(v);
  drawSpr(v, wrx, wry, 0.55f, 1.3f, wreckShape, wreckCol);
  for (int i = 0; i < 9; i++) if (fit[i].on) {
    int scx; float ty; if (!prj(fox, foy, foa, fit[i].x, fit[i].y, scx, ty) || scx < 0 || scx >= SW || ty >= zbc[scx]) continue;
    int H = (int)(SH / ty), yy = 64 + (int)fBob + H / 2 - H * 4 / 10, r = max(2, H / 8); uint16_t col = fit[i].k == 0 ? RGB(255, 230, 90) : RGB(90, 255, 130);
    if (fit[i].k == 0) for (int y2 = 0; y2 < yy; y2 += 2) FB[y2 * SW + scx] = mix(FB[y2 * SW + scx], col, 110);
    cv.fillCircle(scx, yy, r, col); cv.drawCircle(scx, yy, r + 2, C_W);
  }
  if (slMode) drawSpr(v, slx, sly, 1.6f, 0.42f, slendShape, slendCol);
  fxNoise(25 + (int)(fFear * 300)); if (fFear > 0.25f) fxVig(fFear); if (slStare > 1.5f) fxGlitch((int)slStare);
  cv.drawRect(3, 119, 20, 6, RGB(150, 150, 150)); cv.fillRect(23, 121, 2, 2, RGB(150, 150, 150));
  cv.fillRect(4, 120, (int)(18 * fBat), 4, fBat < 0.25f ? RGB(255, 40, 40) : RGB(230, 230, 120));
  cv.drawRect(28, 120, 32, 4, RGB(90, 120, 90)); cv.fillRect(29, 121, (int)(30 * fStam), 2, RGB(90, 220, 110));
  char b[20]; snprintf(b, 20, "PARTS %d/5", fParts); txt(b, SW - 56, 119, RGB(255, 220, 80));
  if (slMode == 2) { cv.drawRect(34, 112, 60, 4, RGB(120, 120, 150)); cv.fillRect(35, 113, (int)(58 * fminf(1, fHideT / 3.5f)), 2, RGB(120, 220, 255)); txtC("STOP + LIGHT OFF", 102, RGB(255, 160, 160)); }
  if (fRep > 0) { cv.drawRect(34, 90, 60, 5, C_W); cv.fillRect(35, 91, (int)(58 * fRep / 3.0f), 3, RGB(120, 255, 160)); }
  if (fMsgT > 0 && fMsg[0]) txtS(fMsg, (SW - (int)strlen(fMsg) * 6) / 2, 6, RGB(255, 235, 200));
}

// ======================= القصص 3D (كاميرا من عيون الشخصية نفسها) =======================
uint8_t scm[40 * 16];
void lutInit() {
  static bool d = false; if (d) return; d = true;
  for (int b = 0; b < 256; b++) { foCL[b] = shc(34, 48, 86, b); foFL[b] = shc(52, 74, 40, b); anCL[b] = shc(70, 72, 70, b); anFL[b] = shc(92, 94, 88, b); }
}
void stBuild(int id) {
  memset(scm, 1, sizeof(scm));
  if (id == 2) {
    for (int y = 7; y <= 9; y++) for (int x = 1; x < 39; x++) scm[y * 40 + x] = 0;
    for (int y = 4; y <= 12; y++) for (int x = 30; x < 39; x++) scm[y * 40 + x] = 0;
  } else if (id == 0) {
    for (int y = 5; y <= 11; y++) for (int x = 1; x < 29; x++) scm[y * 40 + x] = 0;
    for (int y = 3; y <= 13; y++) for (int x = 29; x < 40; x++) scm[y * 40 + x] = 3;
    for (int x = 29; x < 39; x++) scm[8 * 40 + x] = 0;
  }
}
uint16_t stWall(int mx, int my, int side, float u, float v, int br) {
  int t = scm[constrain(my, 0, 15) * 40 + constrain(mx, 0, 39)];
  if (t == 1) return foWall(mx, my, side, u, v, br);
  int row = (int)(v * 14); float uu = u * 6 + ((row & 1) ? 0.5f : 0.0f);
  if (mx == 29 && side == 0 && v > 0.18f && v < 0.42f) {
    float f = fmodf(u * 3.0f, 1.0f);
    if (f > 0.25f && f < 0.75f) return (hh(my * 13) % 4 == 0 && ((int)(stT * 4) % 7 != 0)) ? RGB(235, 185, 80) : shc(8, 8, 14, br);
  }
  if (((int)(v * 56) & 3) == 0 || ((int)(uu * 4) & 3) == 0) return shc(120, 115, 105, br);
  int n = (int)(hh(row * 7 + (int)uu * 13 + my * 5) & 15);
  return shc(125 + n, 58 + n / 2, 44, br);
}
uint8_t eyesShape(float u, float v) { return (inEll(u, v, 0.35f, 0.16f, 0.1f, 0.03f) || inEll(u, v, 0.65f, 0.16f, 0.1f, 0.03f)) ? 1 : 0; }
uint16_t eyesCol(uint8_t k, int br) { return RGB(255, 25, 25); }
uint8_t stFigShape(float u, float v) { return monShape(u, v, true); }
uint16_t stFigCol(uint8_t k, int br) { return k == 1 ? shc(34, 16, 22, br < 50 ? 50 : br) : (k == 2 ? RGB(255, 30, 30) : shc(225, 220, 205, br < 90 ? 90 : br)); }
void stBeam(const FpsV &v, float x, float y) {
  int scx; float ty; if (!prj(v.x, v.y, v.a, x, y, scx, ty) || scx < 0 || scx >= SW || ty >= zbc[scx]) return;
  int H = (int)(SH / ty), yy = v.hz + H / 2 - H * 4 / 10, r = max(2, H / 8);
  for (int y2 = 0; y2 < yy && y2 < 106; y2 += 2) FB[y2 * SW + scx] = mix(FB[y2 * SW + scx], RGB(255, 230, 90), 110);
  cv.fillCircle(scx, yy, r, RGB(255, 230, 90)); cv.drawCircle(scx, yy, r + 2, C_W);
}
void stFade(float k) {
  if (k <= 0) return; uint8_t a = (uint8_t)(k >= 1 ? 255 : k * 255);
  for (int i = 0; i < SW * 106; i++) FB[i] = mix(FB[i], C_K, a);
}
// طبلون العربية (منظور السواق)
void stDash(float t, bool alive, float steer) {
  cv.fillRect(0, 88, SW, 18, RGB(10, 10, 14)); cv.drawFastHLine(0, 88, SW, RGB(46, 46, 54)); cv.fillRect(0, 84, SW, 4, RGB(18, 18, 22));
  cv.drawCircle(24, 98, 8, RGB(80, 80, 92)); float a = alive ? (-2.4f + 1.9f * (0.5f + 0.5f * sinf(t * 0.4f))) : -2.4f;
  cv.drawLine(24, 98, (int)(24 + cosf(a) * 6), (int)(98 + sinf(a) * 6), RGB(255, 80, 60));
  cv.drawCircle(104, 98, 6, RGB(80, 80, 92)); cv.drawPixel(104, 98, alive ? RGB(120, 255, 160) : RGB(255, 60, 60));
  if (alive) {
    int wx = 64 + (int)(steer * 16);
    for (int r = 36; r <= 38; r++) cv.drawCircle(wx, 134, r, RGB(30, 30, 36));
    cv.fillCircle(wx - 24, 103, 3, RGB(205, 160, 130)); cv.fillCircle(wx + 24, 103, 3, RGB(205, 160, 130));
  }
  cv.fillRoundRect(54, 0, 20, 7, 2, RGB(14, 14, 18));
}
void stSky(float ca) {
  vgrad(0, 76, RGB(3, 5, 16), RGB(28, 38, 66));
  for (int k = 0; k < 46; k++) { int x = ((int)(hh(k * 17) % 256) - (int)(ca * 90) + 512) % 256 - 64, y = hh(k * 29) % 60; if ((unsigned)x < SW) { int b = 90 + (hh(k * 5) & 127); px(x, y, RGB(b, b, b)); } }
  int mx = 96 - (int)(ca * 90), my = 20;
  for (int r = 14; r > 7; r--) cv.drawCircle(mx, my, r, mix(RGB(28, 38, 66), RGB(190, 200, 230), (14 - r) * 14));
  cv.fillCircle(mx, my, 7, RGB(232, 232, 210)); cv.fillCircle(mx - 2, my - 2, 2, RGB(205, 205, 185)); cv.fillCircle(mx + 3, my + 2, 1, RGB(205, 205, 185));
}
void fxRain(float t, int n) {
  for (int i = 0; i < n; i++) { int x = hh(i * 17) % 140 - 6; float yy = fmodf(hh(i * 31) * 0.43f + t * (170 + (i & 7) * 12), 112.0f); int y = (int)yy; if (y < 100) cv.drawLine(x, y, x - 2, y + 6, RGB(105, 125, 160)); }
}
void fxDark() {
  for (int y = 0; y < 106; y++) { int dy = y < 105 - y ? y : 105 - y; for (int x = 0; x < SW; x++) { int dx = x < 127 - x ? x : 127 - x, d = dx < dy ? dx : dy; if (d < 6) FB[y * SW + x] = (FB[y * SW + x] >> 2) & 0x39E7; else if (d < 14) FB[y * SW + x] = (FB[y * SW + x] >> 1) & 0x7BEF; } }
}
void stTorch(float t) {
  float b = sinf(t * 8) * 1.5f;
  rotRect(108, 100 + b, 9, 2.6f, -0.6f, RGB(38, 38, 44)); rotRect(114, 96 + b, 3, 3.6f, -0.6f, RGB(95, 95, 105));
  cv.fillCircle(100, 103 + (int)b, 3, RGB(200, 155, 125)); cv.fillCircle(116, 94 + (int)b, 2, RGB(255, 250, 200));
}
void storyDraw() {
  lutInit();
  float t = stT; int line = (int)(t / ST_LINE); if (line >= STN[stId]) line = STN[stId] - 1;
  float tl = t - line * ST_LINE; int n = strlen(STORY[stId][line]);
  if (stId == 1) {   // ---- محطة المترو: بتمشي في الممر ----
    float cx = 2.5f + t * 0.7f, ca = sinf(t * 0.6f) * 0.1f, bob = sinf(t * 6.5f) * 1.2f;
    anKind = (line >= 3) ? 8 : 0; anLevel = 0; anFlick = ((int)(t * 14) % (line >= 3 ? 4 : 13) == 0) ? 0.4f : 1.0f;
    FpsV v = {anm, AN_W, AN_H, cx, 1.6f, ca, 52 + (int)bob, 190, 160, 0.012f, 1.0f, anWall, anCL, anFL, 0, 0, false};
    setCone(0); fpsRender(v);
    if (line >= 4) drawSpr(v, fmaxf(cx + 2.5f, 16.5f - (t - 12.0f) * 0.7f), 1.6f, 1.55f, 0.42f, slendShape, slendCol);
    fxNoise(line >= 3 ? 110 : 25); fxDark(); if (line >= 4) fxVig(0.5f);
  } else if (stId == 0) {   // ---- المصنع القديم: بتمشي ناحية الباب وتدخل ----
    float cx = fminf(2.5f + t * 3.5f, 33.0f), bob = sinf(t * 8) * 1.6f * (cx < 33 ? 1 : 0), ca = sinf(t * 0.7f) * 0.05f + (line >= 3 ? sinf(t * 7) * 0.02f : 0);
    bool inside = cx >= 28.5f; float fl = (hh((int)(t * 12)) % 9 == 0) ? 0.3f : 1.0f;
    int amb = inside ? 38 : 125;
    FpsV v = {scm, 40, 16, cx, 8.5f, ca, 52 + (int)bob, amb, amb - 10, 0.05f, 2.0f, stWall, foCL, foFL, RGB(18, 24, 36), 12, false};
    if (!inside) { stSky(ca); gSkipCeil = true; }
    setCone(inside ? 0.85f * fl : 0.0f); fpsRender(v); gSkipCeil = false;
    if (line >= 3) drawSpr(v, 38.5f, 8.5f, 1.0f, 0.4f, eyesShape, eyesCol);
    if (line >= 4) drawSpr(v, 38.8f - (t - 12.0f) * 1.2f, 8.5f, 1.05f, 0.55f, stFigShape, stFigCol);
    if (!inside) fxRain(t, 38);
    fxNoise(line >= 3 ? 60 + (int)(tl * 60) : 20); fxDark(); if (line >= 3) fxVig(0.4f);
    if (inside) stTorch(t);
  } else {   // ---- الحادثة: من عيون السواق ----
    const float TC = 7.4f;
    float cx, cy = 8.5f, ca = 0, hz = 52, steer = 0; bool crashed = t >= TC;
    float cxc = 20.0f + (TC - 6.0f) * 3.0f, cyc = 7.25f, cac = -0.37f;
    if (!crashed) {
      cx = 3.0f + fminf(t, 6.0f) * 3.0f;
      if (t > 6.0f) { float s = (t - 6.0f); cx = 21.0f + s * 3.0f; cy = fmaxf(7.25f, 8.5f - s * s * 0.9f); ca = -(s / 1.4f) * (s / 1.4f) * 0.37f; steer = -fminf(1.0f, s * 1.2f); }
      hz = 52 + sinf(t * 17) * (t > 6.0f ? 3.0f : 0.5f);
    } else {
      cx = cxc; cy = cyc; ca = cac;
      if (line == 3) { hz = 70 + sinf(t * 1.1f) * 3; ca = cac + sinf(t * 0.6f) * 0.05f; }
      else { float k = constrain((t - 12.0f) / 2.0f, 0.0f, 1.0f); hz = 70 - 18 * k; ca = cac + (0.0f - cac) * k; cx = cxc + fmaxf(0, t - 12.0f) * 1.1f; cy = cyc + (8.2f - cyc) * k; }
    }
    float torch = !crashed ? 0.95f : ((hh((int)(t * 9)) % 3 == 0) ? 0.1f : 0.4f); if (crashed && line >= 4) torch = (hh((int)(t * 9)) % 5 == 0) ? 0.1f : 0.8f;
    FpsV v = {scm, 40, 16, cx, cy, ca, (int)hz, 85, 75, 0.045f, 2.4f, stWall, foCL, foFL, RGB(20, 30, 44), 15, false};
    stSky(ca); gSkipCeil = true; setCone(torch); fpsRender(v); gSkipCeil = false;
    if (!crashed && line >= 1 && t > 3.4f) drawSpr(v, fminf(cx + 12.0f, 37.0f) - fminf(0.6f * (t - 3.4f), 3.0f), 8.0f, 1.6f, 0.42f, slendShape, slendCol);
    if (line >= 4) { stBeam(v, 30.5f, 6.5f); stBeam(v, 32.5f, 10.5f); stBeam(v, 34.5f, 5.5f); stBeam(v, 36.5f, 9.5f); stBeam(v, 37.5f, 7.5f); }
    if (line >= 5) drawSpr(v, fminf(cx + 8.0f, 37.5f), 8.0f, 1.6f, 0.42f, slendShape, slendCol);
    fxRain(t, crashed ? 30 : 46);
    if (!crashed) stDash(t, true, steer);
    else if (line == 3) {
      stDash(t, false, 0);
      uint16_t cr = RGB(210, 220, 230);
      cv.drawLine(88, 6, 70, 30, cr); cv.drawLine(88, 6, 110, 24, cr); cv.drawLine(88, 6, 96, 44, cr); cv.drawLine(70, 30, 52, 36, cr); cv.drawLine(96, 44, 112, 60, cr); cv.drawLine(88, 6, 60, 8, cr);
      for (int k = 0; k < 5; k++) cv.fillCircle(30 + k * 18 + (int)(sinf(t * 1.3f + k) * 4), 96 - ((int)(t * 14 + k * 9) % 40), 4 + (k & 1), RGB(46, 46, 54));
    }
    if (line >= 4) { stTorch(t); fxNoise(10 + (line - 4) * 80); }
    if (line >= 5) fxVig(0.45f);
    fxDark();
    if (t < TC) { if (t > 6.0f) { fxNoise(80); fxGlitch(2); } } else stFade(t < 8.2f ? 1.0f : 1.0f - (t - 8.2f) / 1.4f);
  }
  cv.fillRect(0, 0, SW, 5, C_K);
  cv.fillRect(0, 106, SW, 22, C_K); cv.drawFastHLine(0, 106, SW, RGB(70, 20, 20));
  { const char *sb = STORY[stId][line]; int tot = strlen(sb), k = (int)(tl * 18); if (k > tot) k = tot;
    char r1[24] = {0}, r2[24] = {0}; int c1 = 0, c2 = 0, L1 = 0, L2 = 0; bool sec = false;
    for (int q = 0; q < tot; q++) { if (sb[q] == '|') { sec = true; continue; } if (!sec) L1++; else L2++; }
    sec = false;
    for (int q = 0; q < k; q++) { if (sb[q] == '|') { sec = true; continue; } if (!sec) { if (c1 < 22) r1[c1++] = sb[q]; } else if (c2 < 22) r2[c2++] = sb[q]; }
    txt(r1, (SW - L1 * 6) / 2, 109, RGB(235, 235, 235)); txt(r2, (SW - L2 * 6) / 2, 118, RGB(235, 235, 235));
    txt("START:SKIP", 66, 7, RGB(110, 110, 120)); }
}

// ======================= جدول الألعاب =======================
struct Game { const char *name; uint16_t col; bool hud; void (*init)(); void (*upd)(float); void (*drw)(); };
const Game G[] = {
  {"SNAKE", RGB(60, 255, 140), true, snInit, snUpd, snDraw},
  {"BREAKOUT", RGB(255, 150, 40), true, bkInit, bkUpd, bkDraw},
  {"FLAPPY", RGB(255, 220, 40), true, fInit, fUpd, fDraw},
  {"NEON DASH", RGB(80, 160, 255), false, rInit, rUpd, rDraw},
  {"STARFIRE", RGB(70, 170, 255), true, shInit, shUpd, shDraw},
  {"TETRIS", RGB(180, 70, 240), false, tInit, tUpd, tDraw},
  {"PONG", RGB(0, 255, 200), true, pgInit, pgUpd, pgDraw},
  {"RACER 3D", RGB(255, 100, 150), false, rcInit, rcUpd, rcDraw},
  {"NIGHTMARE", RGB(220, 30, 30), false, hInit, hUpd, hDraw},
  {"ASTEROIDS", RGB(120, 220, 255), true, asInit, asUpd, asDraw},
  {"CRAFT 3D", RGB(90, 200, 90), false, crInit, crUpd, crDraw},
  {"HIDE&SEEK", RGB(255, 180, 60), false, hsInit, hsUpd, hsDraw},
  {"ANOMALY", RGB(200, 200, 225), false, anInit, anUpd, anDraw},
  {"LOST ROAD", RGB(120, 180, 120), false, foInit, foUpd, foDraw},
};
const int NG = sizeof(G) / sizeof(G[0]);
const int NM = NG + 1;

void die() {
  if (over) return;
  over = true; shake = 0.4f; newBest = false;
  if (score > hiS[cur]) { hiS[cur] = score; newBest = true; char k[4]; snprintf(k, 4, "h%d", cur); pref.putUInt(k, score); }
  if (won) SFX(SFX_WIN); else SFX(SFX_DIE);
}
void startGame(int i) {
  cur = i; state = S_PLAY; score = 0; lives = 0; over = false; paused = false; newBest = false; won = false; shake = 0; noPause = false; oledX[0] = 0;
  for (auto &p : pts) p.life = 0; audioFlush(); G[i].init();
}
void onLeave() {   // حفظ تلقائي عند الخروج من أي لعبة
  if (state != S_PLAY) return;
  if (cur == GM_CRAFT) crSave();
  else if (cur == GM_FOREST) foSave();
  else if (cur == GM_HORROR) hSaveNow();
  else if (cur == GM_ANOM) anSave();
  else if (cur == GM_RACER) rcLeave();
}

// ======================= Deep Sleep =======================
void goSleep() {
  onLeave();
  SFX(SFX_SLEEP);
  uint32_t t0 = millis(); delay(30); while (!audioIdle() && millis() - t0 < 1500) delay(10);
  audioMute = true; delay(60); audioHalt = true; delay(40);
  if (spkOk) { i2s_zero_dma_buffer(I2S_NUM_0); i2s_stop(I2S_NUM_0); i2s_driver_uninstall(I2S_NUM_0); spkOk = false; }
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0); ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7); ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_7, 0);
  for (int p = cfg.bl; p > 0; p -= 5) { BL_WRITE((p * p * 255) / 10000); delay(14); }
  BL_WRITE(0);
  sleeping = true; vTaskDelay(pdMS_TO_TICKS(120));
  xSemaphoreTake(semDone, portMAX_DELAY);
  tft.sendCommand(ST77XX_DISPOFF); delay(5); tft.sendCommand(ST77XX_SLPIN);
  oled.ssd1306_command(SSD1306_DISPLAYOFF);
  saveCfgNow();
  int pin = PIN_BTN[cfg.wakeIdx];
  while (digitalRead(pin) == LOW) delay(10);
  delay(80);
  BL_DETACH(); pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, LOW);
  gpio_hold_en((gpio_num_t)TFT_BL); gpio_deep_sleep_hold_en();
  gpio_reset_pin((gpio_num_t)BUZZER_PIN); pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW); gpio_hold_en((gpio_num_t)BUZZER_PIN);
  rtc_gpio_init((gpio_num_t)pin); rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis((gpio_num_t)pin); rtc_gpio_pullup_en((gpio_num_t)pin);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  esp_sleep_enable_ext1_wakeup(1ULL << pin, ESP_EXT1_WAKEUP_ALL_LOW);
#endif
  esp_deep_sleep_start();
}

// ======================= الـOLED (Core 0) - الأصفر 0..15 للحالة، الأزرق 16..63 للمحتوى =======================
void oledDraw() {
  bool bk = (millis() / 500) & 1; char b[28];
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.drawRect(0, 0, 16, 7, SSD1306_WHITE); oled.fillRect(16, 2, 2, 3, SSD1306_WHITE);
  if (!noBat) oled.fillRect(2, 2, (batPct * 12) / 100, 3, SSD1306_WHITE);
  oled.setCursor(22, 0);
  if (lowBat && bk) oled.print("LOW BATTERY");
  else if (noBat) oled.print("USB POWER");
  else { snprintf(b, sizeof(b), "%.2fV %d%%%s", (double)batV, (int)batPct, charging ? "+" : ""); oled.print(b); }
  if (hot && bk) snprintf(b, sizeof(b), "HOT!"); else snprintf(b, sizeof(b), "%.1fC", (double)tempShow);
  oled.setCursor(128 - strlen(b) * 6, 0); oled.print(b);
  oled.setCursor(0, 8);
  if (state == S_MENU) oled.print("NEON ARCADE");
  else if (state == S_PLAY) { oled.print(G[cur].name); snprintf(b, sizeof(b), "HI %d", hiS[cur]); oled.setCursor(128 - strlen(b) * 6, 8); oled.print(b); }
  else oled.print("SETTINGS");
  if (state == S_MENU) {
    const char *nm = sel < NG ? G[sel].name : "SETTINGS";
    oled.setTextSize(2); oled.setCursor((128 - strlen(nm) * 12) / 2, 20); oled.print(nm);
    oled.setTextSize(1); oled.setCursor(0, 41);
    if (sel < NG) { oled.print("HI SCORE: "); oled.print(hiS[sel]); } else oled.print("BRIGHT KEYS BAT TEMP");
    oled.setCursor(0, 55); oled.print("<> MOVE      A: OK");
  } else if (state == S_PLAY) {
    int bx = 0;
    if (cur == GM_HORROR && !over && hPh == 2) {
      for (int y = 0; y < HM; y++) for (int x = 0; x < HM; x++) if (hseen[y][x]) { if (hmap[y][x]) oled.fillRect(2 + x * 2, 18 + y * 2, 2, 2, SSD1306_WHITE); }
      if (bk) oled.fillRect(2 + (int)(hpx * 2) - 1, 18 + (int)(hpy * 2) - 1, 3, 3, SSD1306_WHITE);
      if (hFuses >= hNeed && bk) oled.drawRect(2 + 19 * 2 - 1, 18 + 19 * 2 - 1, 4, 4, SSD1306_WHITE);
      bx = 50;
    }
    oled.setTextSize(cur == GM_HORROR && bx ? 2 : 3); oled.setCursor(bx, 22); oled.print(score);
    if (cur == GM_DASH) { oled.setTextSize(1); oled.print("%"); }
    oled.setTextSize(1); oled.setCursor(bx, 50);
    if (cur == GM_HORROR && !over && hPh == 2) { snprintf(b, sizeof(b), "FUSE %d/%d", hFuses, hNeed); oled.print(b); }
    else if (oledX[0]) oled.print(oledX);
    oled.setCursor(0, 55);
    if (bx) oled.setCursor(bx, 55);
    if (over) oled.print(newBest ? "** NEW BEST **" : (won ? "COMPLETE!" : "GAME OVER"));
    else if (paused) oled.print("PAUSED");
    else { snprintf(b, sizeof(b), "%d FPS", (int)fps); oled.print(b); }
    for (int i = 0; i < lives; i++) oled.fillCircle(122 - i * 9, 59, 3, SSD1306_WHITE);
  } else {
    oled.setCursor(0, 20); oled.print(oledHelp);
  }
  oled.display();
}
void infoTask(void *) {
  uint32_t tS = 0, tO = 0;
  for (;;) {
    if (!sleeping) {
      uint32_t n = millis();
      bool pl = (state == S_PLAY); uint32_t iS = pl ? 3000 : 400, iO = pl ? 250 : 100;
      if (n - tS >= iS) { tS = n; readSensors(); }
      if (oledCtrDirty) { oledCtrDirty = false; oled.ssd1306_command(SSD1306_SETCONTRAST); oled.ssd1306_command(cfg.oledC); }
      if (n - tO >= iO) { tO = n; oledDraw(); }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ======================= الإعدادات =======================
enum { I_BL, I_OLED, I_SOUND, I_VOL, I_SPK, I_SENS, I_MUSIC, I_HUD, I_SLEEPT, I_WAKE, I_CPU, I_SPI, I_FIT, I_KEYS, I_BATCAL, I_TEMPCAL, I_INFO, I_SLEEPNOW, I_RSCORE, I_RSAVE, I_FACTORY, I_COUNT };
const char *SET_LBL[I_COUNT] = {"BRIGHT", "OLED LUM", "SOUND", "VOLUME", "SPEAKER", "TURN SPD", "MUSIC", "TFT HUD", "AUTOSLEEP", "WAKE KEY", "CPU", "SPI CLK", "SCREEN FIT", "KEY MAP", "BAT CAL", "TEMP CAL", "SYS INFO", "SLEEP NOW", "CLR SCORES", "CLR SAVES", "FACTORY"};
const char *SET_HELP[I_COUNT] = {
  "TFT BACKLIGHT\n<> CHANGE LEVEL", "OLED CONTRAST\n<> CHANGE LEVEL", "ALL SOUNDS\n<> ON / OFF", "BUZZER LOUDNESS\n<> 1..5", "SPEAKER MODE\n<> ON / OFF\nOFF = OLD BUZZER", "TURN / STEER SPEED\n<> 1..5\nHIGHER = FASTER",
  "MENU MELODY\n<> ON / OFF",
  "GAME SCREEN INFO\nFULL MINI OFF\nOLED SHOWS THE REST", "IDLE TIME BEFORE\nDEEP SLEEP", "BUTTON THAT WAKES\nTHE DEVICE",
  "CPU SPEED\nLOWER = LESS HEAT", "TFT SPI SPEED\nLOWER IF NOISE", "A: FIX EDGES AND\nOFFSET OF TFT", "A: ASSIGN BUTTONS\nUP DOWN A B ...",
  "A: CALIBRATE\nBATTERY VOLTAGE", "A: CALIBRATE\nTEMPERATURE", "A: LIVE SYSTEM\nINFORMATION", "A: SLEEP NOW", "A TWICE: RESET\nALL HIGH SCORES", "A TWICE: ERASE\nCRAFT+HORROR SAVES", "A TWICE: ERASE\nALL SETTINGS"};
enum { SP_LIST, SP_FIT, SP_KEYS, SP_BAT, SP_TEMP, SP_INFO };
int spage = SP_LIST, setSel = 0, setTop = 0, confItem = -1; float confT = 0;
int8_t fitOldC, fitOldR; int keyStep; uint8_t newMap[8]; float keyT, calV, calT;
const char *ACT_LONG[8] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "START", "SELECT"};
const char *ACT_SHORT[8] = {"UP", "DN", "LF", "RT", "A", "B", "ST", "SE"};
#define VR 7

void itemVal(int i, char *b, int n) {
  const char *h[3] = {"FULL", "MINI", "OFF"};
  switch (i) {
    case I_BL: snprintf(b, n, "%d%%", cfg.bl); break;
    case I_OLED: snprintf(b, n, "%d", cfg.oledC); break;
    case I_SOUND: snprintf(b, n, "%s", cfg.sound ? "ON" : "OFF"); break;
    case I_VOL: snprintf(b, n, "%d/5", cfg.vol); break;
    case I_SENS: snprintf(b, n, "%d/5", sens); break;
    case I_SPK: snprintf(b, n, "%s", (spkMode && spkOk) ? "ON" : "OFF"); break;
    case I_MUSIC: snprintf(b, n, "%s", cfg.music ? "ON" : "OFF"); break;
    case I_HUD: snprintf(b, n, "%s", h[cfg.hud % 3]); break;
    case I_SLEEPT: if (cfg.sleepIdx) snprintf(b, n, "%d MIN", SLEEP_MIN[cfg.sleepIdx]); else snprintf(b, n, "OFF"); break;
    case I_WAKE: snprintf(b, n, "%s", BTN_NAME[cfg.wakeIdx]); break;
    case I_CPU: snprintf(b, n, "%dMHz", CPU_MHZ[cfg.cpuIdx]); break;
    case I_SPI: snprintf(b, n, "%dMHz", SPI_MHZ[cfg.spiIdx]); break;
    default: snprintf(b, n, ">"); break;
  }
}
void adjust(int i, int d) {
  switch (i) {
    case I_BL: cfg.bl = constrain((int)cfg.bl + d * 5, 10, 100); applyBL(); break;
    case I_OLED: cfg.oledC = constrain((int)cfg.oledC + d * 16, 0, 255); oledCtrDirty = true; break;
    case I_SOUND: cfg.sound = !cfg.sound; break;
    case I_VOL: cfg.vol = constrain((int)cfg.vol + d, 1, 5); break;
    case I_SENS: sens = constrain((int)sens + d, 1, 5); pref.putUChar("sens", sens); break;
    case I_SPK: spkMode = !spkMode; pref.putUChar("spk", spkMode); break;
    case I_MUSIC: cfg.music = !cfg.music; break;
    case I_HUD: cfg.hud = (cfg.hud + 3 + d) % 3; break;
    case I_SLEEPT: cfg.sleepIdx = (cfg.sleepIdx + 5 + d) % 5; break;
    case I_WAKE: cfg.wakeIdx = (cfg.wakeIdx + 3 + d) % 3; break;
    case I_CPU: cfg.cpuIdx = (cfg.cpuIdx + 3 + d) % 3; setCpuFrequencyMhz(CPU_MHZ[cfg.cpuIdx]); break;
    case I_SPI: cfg.spiIdx = (cfg.spiIdx + 3 + d) % 3;
      xSemaphoreTake(semDone, portMAX_DELAY); tft.setSPISpeed(SPI_MHZ[cfg.spiIdx] * 1000000UL); xSemaphoreGive(semDone); break;
    default: return;
  }
  touchCfg(); SFX(SFX_MOVE);
}
void drawTitle(const char *t) {
  vgrad(0, 14, RGB(20, 30, 90), RGB(10, 10, 40)); cv.drawFastHLine(0, 14, SW, RGB(0, 200, 255)); txtC(t, 3, C_W);
}
void act(int i) {
  switch (i) {
    case I_SOUND: case I_SPK: case I_MUSIC: case I_HUD: adjust(i, 1); break;
    case I_FIT: fitOldC = cfg.colS; fitOldR = cfg.rowS; spage = SP_FIT; break;
    case I_KEYS: spage = SP_KEYS; keyStep = 0; keyT = 0; break;
    case I_BATCAL: calV = roundf(batV * 100) / 100.0f; spage = SP_BAT; break;
    case I_TEMPCAL: calT = roundf(tempC * 2) / 2.0f; spage = SP_TEMP; break;
    case I_INFO: spage = SP_INFO; break;
    case I_SLEEPNOW: goSleep(); break;
    case I_RSCORE: case I_RSAVE: case I_FACTORY:
      if (confItem != i) { confItem = i; confT = 3; SFX(SFX_MOVE); break; }
      if (i == I_RSCORE) { for (int k = 0; k < NG; k++) { hiS[k] = 0; char kk[4]; snprintf(kk, 4, "h%d", k); pref.putUInt(kk, 0); } SFX(SFX_OK); }
      else if (i == I_RSAVE) { pref.remove("cw"); pref.remove("fsv"); pref.remove("nsv"); pref.remove("asv"); SFX(SFX_OK); }
      else { pref.clear(); SFX(SFX_DIE); delay(600); ESP.restart(); }
      confItem = -1; break;
  }
}
void drawList() {
  vgrad(0, SH, RGB(8, 10, 28), RGB(18, 8, 36)); drawTitle("SETTINGS");
  for (int r = 0; r < VR; r++) {
    int i = setTop + r; if (i >= I_COUNT) break;
    int y = 18 + r * 14; bool s = (i == setSel);
    if (s) { cv.fillRoundRect(3, y, 118, 13, 3, RGB(0, 70, 130)); cv.drawRoundRect(3, y, 118, 13, 3, RGB(0, 200, 255)); }
    char v[16]; itemVal(i, v, sizeof(v)); if (confItem == i) snprintf(v, sizeof(v), "SURE?");
    txt(SET_LBL[i], 8, y + 3, s ? C_W : RGB(150, 160, 200));
    txt(v, 116 - strlen(v) * 6, y + 3, confItem == i ? RGB(255, 220, 60) : (s ? RGB(120, 255, 160) : RGB(100, 170, 130)));
  }
  int th = 98 * VR / I_COUNT, ty = 18 + (98 - th) * setTop / (I_COUNT - VR);
  cv.fillRoundRect(124, 18, 2, 98, 1, RGB(30, 36, 70)); cv.fillRoundRect(124, ty, 2, th, 1, RGB(0, 200, 255));
  txtC("<>:ADJ A:OK B:BACK", 119, RGB(120, 130, 190));
}
void setList(float dt) {
  int old = setSel;
  if (rpt[K_UP]) { setSel = (setSel + I_COUNT - 1) % I_COUNT; SFX(SFX_MOVE); }
  if (rpt[K_DN]) { setSel = (setSel + 1) % I_COUNT; SFX(SFX_MOVE); }
  if (setSel != old) confItem = -1;
  if (setSel < setTop) setTop = setSel;
  if (setSel >= setTop + VR) setTop = setSel - VR + 1;
  if (rpt[K_LF]) adjust(setSel, -1);
  if (rpt[K_RT]) adjust(setSel, 1);
  if (prs[K_A]) act(setSel);
  if (confItem >= 0) { confT -= dt; if (confT <= 0) confItem = -1; }
  if (prs[K_B] || prs[K_SE]) { saveCfgNow(); state = S_MENU; confItem = -1; SFX(SFX_BACK); return; }
  oledHelp = SET_HELP[setSel];
  if (spage == SP_LIST) drawList();
}
void setFit(float dt) {
  bool ch = false;
  if (rpt[K_RT] && cfg.colS < 40) { cfg.colS++; ch = true; }
  if (rpt[K_LF] && cfg.colS > 0) { cfg.colS--; ch = true; }
  if (rpt[K_DN] && cfg.rowS < 40) { cfg.rowS++; ch = true; }
  if (rpt[K_UP] && cfg.rowS > 0) { cfg.rowS--; ch = true; }
  if (ch) { applyTft(); SFX(SFX_TICK); }
  if (prs[K_A]) { touchCfg(); saveCfgNow(); SFX(SFX_OK); spage = SP_LIST; return; }
  if (prs[K_B]) { cfg.colS = fitOldC; cfg.rowS = fitOldR; applyTft(); spage = SP_LIST; return; }
  cv.fillScreen(RGB(0, 0, 30));
  for (int i = 0; i < 128; i += 8) { cv.drawFastHLine(0, i, 128, RGB(20, 20, 60)); cv.drawFastVLine(i, 0, 128, RGB(20, 20, 60)); }
  cv.drawLine(0, 0, 127, 127, RGB(60, 60, 120)); cv.drawLine(127, 0, 0, 127, RGB(60, 60, 120));
  cv.drawRect(0, 0, 128, 128, RGB(255, 0, 0)); cv.drawRect(1, 1, 126, 126, RGB(255, 255, 0));
  cv.fillRect(0, 0, 6, 6, C_W); cv.fillRect(122, 0, 6, 6, RGB(0, 255, 0)); cv.fillRect(0, 122, 6, 6, RGB(0, 150, 255)); cv.fillRect(122, 122, 6, 6, RGB(255, 0, 255));
  txtC("TOP", 8, C_W); txtC("BOTTOM", 112, C_W); txt("L", 8, 60, C_W); txt("R", 114, 60, C_W);
  cv.fillRoundRect(24, 38, 80, 40, 4, RGB(0, 0, 30)); cv.drawRoundRect(24, 38, 80, 40, 4, RGB(0, 200, 255));
  char b[24]; snprintf(b, 24, "X:%d  Y:%d", cfg.colS, cfg.rowS);
  txtC("SCREEN FIT", 43, RGB(0, 255, 255)); txtC(b, 55, RGB(255, 255, 0)); txtC("D-PAD: MOVE", 66, RGB(180, 180, 255));
  cv.fillRect(8, 86, 112, 20, RGB(0, 0, 30));
  txtC("SEE ALL 4 BORDERS", 88, C_W); txtC("A:SAVE  B:CANCEL", 97, RGB(120, 255, 160));
  snprintf(oledDyn, sizeof(oledDyn), "X=%d Y=%d\nMOVE UNTIL ALL 4\nBORDERS ARE VISIBLE", cfg.colS, cfg.rowS); oledHelp = oledDyn;
}
void setKeys(float dt) {
  keyT += dt;
  for (int i = 0; i < 8; i++) if (rawP[i]) {
    bool used = false; for (int j = 0; j < keyStep; j++) if (newMap[j] == i) used = true;
    if (used) { snd(200, 60); continue; }
    newMap[keyStep++] = i; SFX(SFX_MOVE); keyT = 0; break;
  }
  if (keyStep >= 8) { memcpy(cfg.map, newMap, 8); touchCfg(); saveCfgNow(); SFX(SFX_OK); spage = SP_LIST; return; }
  if (keyT > 10) { spage = SP_LIST; SFX(SFX_BACK); return; }
  vgrad(0, SH, RGB(8, 10, 28), RGB(18, 8, 36)); drawTitle("KEY MAP");
  txtC("PRESS BUTTON FOR", 20, RGB(0, 220, 255));
  txtRainbow(ACT_LONG[keyStep], 32, 3, millis() / 8);
  for (int k = 0; k < 8; k++) {
    int x = (k < 4) ? 8 : 70, y = 62 + (k % 4) * 10; char b[16];
    if (k < keyStep) snprintf(b, 16, "%s:%s", ACT_SHORT[k], BTN_NAME[newMap[k]]); else snprintf(b, 16, "%s:--", ACT_SHORT[k]);
    txt(b, x, y, k == keyStep ? RGB(255, 255, 0) : (k < keyStep ? RGB(120, 255, 160) : RGB(100, 110, 150)));
  }
  txtC("IDLE 10s = CANCEL", 104, RGB(120, 130, 190));
  cv.drawRoundRect(6, 115, 116, 6, 2, RGB(80, 90, 150)); cv.fillRoundRect(7, 116, (int)(114 * (10 - keyT) / 10), 4, 1, RGB(0, 200, 255));
  snprintf(oledDyn, sizeof(oledDyn), "PRESS PHYSICAL KEY\nFOR: %s\n(%d / 8)", ACT_LONG[keyStep], keyStep + 1); oledHelp = oledDyn;
}
void setBat(float dt) {
  if (rpt[K_UP]) calV += 0.01f;
  if (rpt[K_DN]) calV -= 0.01f;
  if (rpt[K_RT]) calV += 0.1f;
  if (rpt[K_LF]) calV -= 0.1f;
  calV = constrain(calV, 2.5f, 4.35f);
  if (prs[K_A]) {
    if (batPinV > 0.2f) { cfg.vfac = calV / batPinV; batInit = false; touchCfg(); saveCfgNow(); SFX(SFX_OK); }
    spage = SP_LIST; return;
  }
  if (prs[K_B]) { spage = SP_LIST; return; }
  vgrad(0, SH, RGB(8, 10, 28), RGB(18, 8, 36)); drawTitle("BATTERY CAL");
  char b[24];
  txtC("MEASURED", 22, RGB(0, 220, 255)); snprintf(b, 24, "%.2fV", (double)batV); txtC(b, 33, RGB(255, 220, 60), 2);
  txtC("MULTIMETER", 58, RGB(0, 220, 255)); snprintf(b, 24, "%.2fV", calV); txtC(b, 69, RGB(120, 255, 160), 2);
  txtC("UP/DN .01  L/R .1", 92, RGB(150, 160, 210)); txtC("A:APPLY  B:BACK", 106, C_W);
  snprintf(oledDyn, sizeof(oledDyn), "SET THE VALUE OF\nYOUR MULTIMETER\nTHEN PRESS A"); oledHelp = oledDyn;
}
void setTemp(float dt) {
  if (rpt[K_UP] || rpt[K_RT]) calT += 0.5f;
  if (rpt[K_DN] || rpt[K_LF]) calT -= 0.5f;
  calT = constrain(calT, -10, 80);
  if (prs[K_A]) { cfg.toff = tempRaw - calT; tempShow = calT; touchCfg(); saveCfgNow(); SFX(SFX_OK); spage = SP_LIST; return; }
  if (prs[K_B]) { spage = SP_LIST; return; }
  vgrad(0, SH, RGB(8, 10, 28), RGB(18, 8, 36)); drawTitle("TEMP CAL");
  char b[24];
  txtC("SHOWN NOW", 22, RGB(0, 220, 255)); snprintf(b, 24, "%.1fC", (double)tempShow); txtC(b, 33, RGB(255, 220, 60), 2);
  txtC("REAL TEMP", 58, RGB(0, 220, 255)); snprintf(b, 24, "%.1fC", calT); txtC(b, 69, RGB(120, 255, 160), 2);
  snprintf(b, 24, "CHIP RAW: %.1fC", (double)tempRaw); txtC(b, 92, RGB(150, 160, 210)); txtC("A:APPLY  B:BACK", 106, C_W);
  snprintf(oledDyn, sizeof(oledDyn), "SENSOR = CHIP TEMP\nSET REAL ROOM TEMP\nTHEN PRESS A"); oledHelp = oledDyn;
}
void setInfo(float dt) {
  if (prs[K_A] || prs[K_B]) { spage = SP_LIST; return; }
  vgrad(0, SH, RGB(8, 10, 28), RGB(18, 8, 36)); drawTitle("SYSTEM INFO");
  char b[32]; int y = 18; uint16_t c = RGB(170, 220, 255);
  snprintf(b, 32, "FPS %d  CPU %dMHz", (int)fps, (int)getCpuFrequencyMhz()); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "BAT %.2fV %d%%%s", (double)batV, (int)batPct, charging ? " CHG" : ""); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "PIN %.3fV  F %.3f", (double)batPinV, (double)cfg.vfac); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "TEMP %.1fC RAW %.1f", (double)tempShow, (double)tempRaw); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "HEAP %uKB MIN %uKB", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024)); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "FLASH %uMB PS %uMB", (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)(ESP.getPsramSize() >> 20)); txt(b, 4, y, c); y += 10;
  snprintf(b, 32, "UP %lus", (unsigned long)(millis() / 1000)); txt(b, 4, y, c); y += 10;
  strcpy(b, "KEYS:"); for (int i = 0; i < 8; i++) if (rawH[i]) { strcat(b, " "); strcat(b, BTN_NAME[i] + 1); }
  txt(b, 4, y, RGB(255, 220, 80));
  txtC("A/B: BACK", 118, RGB(120, 130, 190));
  snprintf(oledDyn, sizeof(oledDyn), "%s\nA / B : BACK", ESP.getChipModel()); oledHelp = oledDyn;
}
void settingsFrame(float dt) {
  switch (spage) {
    case SP_LIST: setList(dt); break;
    case SP_FIT: setFit(dt); break;
    case SP_KEYS: setKeys(dt); break;
    case SP_BAT: setBat(dt); break;
    case SP_TEMP: setTemp(dt); break;
    case SP_INFO: setInfo(dt); break;
  }
}

// ======================= القائمة واللعب =======================
void icon(int i, int cx, int cy, uint16_t c) {
  float t = millis() * 0.005f;
  switch (i) {
    case GM_SNAKE: for (int k = 0; k < 7; k++) cv.fillRoundRect(cx - 18 + k * 5, cy - 3 + (int)(sinf(k * 0.9f + t) * 5), 6, 6, 2, mix(c, C_K, k * 22));
            cv.fillCircle(cx + 17, cy + 9, 3, RGB(255, 50, 70)); break;
    case GM_BREAK: for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) bevel(cx - 20 + k * 10, cy - 14 + r * 6, 9, 5, bkCol[r + k % 2]);
            cv.fillRoundRect(cx - 8, cy + 12, 16, 3, 1, C_W); cv.fillCircle(cx + (int)(sinf(t) * 10), cy + 5, 2, C_W); break;
    case GM_FLAPPY: { int by = cy + (int)(sinf(t * 1.5f) * 3); cv.fillCircle(cx, by, 9, c); cv.fillCircle(cx - 3, by + 2, 4, RGB(255, 140, 20));
            cv.fillCircle(cx + 4, by - 3, 3, C_W); cv.drawPixel(cx + 5, by - 3, C_K); cv.fillTriangle(cx + 8, by, cx + 14, by + 2, cx + 8, by + 5, RGB(255, 90, 40)); } break;
    case GM_DASH: { int j = (int)(fabsf(sinf(t * 0.8f)) * 12); rotSq(cx - 10, cy + 4 - j, 5.5f, j > 1 ? t * 3 : 0, C_K); rotSq(cx - 10, cy + 4 - j, 4.6f, j > 1 ? t * 3 : 0, RGB(120, 255, 0)); rotSq(cx - 10, cy + 4 - j, 2.6f, j > 1 ? t * 3 : 0, RGB(0, 235, 255));
            for (int k = 0; k < 2; k++) { cv.fillTriangle(cx + 4 + k * 8, cy + 10, cx + 8 + k * 8, cy - 2, cx + 12 + k * 8, cy + 10, C_K); cv.drawTriangle(cx + 4 + k * 8, cy + 10, cx + 8 + k * 8, cy - 2, cx + 12 + k * 8, cy + 10, C_W); }
            cv.drawFastHLine(cx - 20, cy + 10, 40, C_W); } break;
    case GM_STAR: cv.fillTriangle(cx, cy - 12, cx - 9, cy + 9, cx + 9, cy + 9, c); cv.fillCircle(cx, cy, 2, RGB(0, 255, 255));
            cv.fillTriangle(cx - 2, cy + 10, cx + 2, cy + 10, cx, cy + 14 + (int)(t * 7) % 3, RGB(255, 150, 30)); cv.fillRect(cx - 1, cy - 18 - (int)(t * 9) % 4, 2, 4, RGB(255, 240, 100)); break;
    case GM_TETRIS: for (int k = 0; k < 3; k++) bevel(cx - 10 + k * 7, cy + 2, 7, 7, TC[3]); bevel(cx - 3, cy - 5, 7, 7, TC[3]); bevel(cx - 17, cy + 9, 7, 7, TC[1]); break;
    case GM_PONG: cv.fillRoundRect(cx - 20, cy - 9, 4, 18, 2, RGB(0, 255, 200)); cv.fillRoundRect(cx + 16, cy - 9, 4, 18, 2, RGB(255, 80, 160));
            cv.fillCircle(cx + (int)(sinf(t) * 12), cy + (int)(cosf(t * 1.7f) * 8), 3, C_W); break;
    case GM_RACER:
      cv.fillTriangle(cx, cy - 14, cx - 22, cy + 14, cx + 22, cy + 14, RGB(40, 36, 70));
      cv.drawLine(cx, cy - 14, cx - 22, cy + 14, RGB(0, 255, 255)); cv.drawLine(cx, cy - 14, cx + 22, cy + 14, RGB(0, 255, 255));
      for (int k = 0; k < 3; k++) { int yy = cy - 6 + ((int)(t * 6) + k * 8) % 24; cv.drawFastVLine(cx, yy, 3, C_W); }
      cv.fillRoundRect(cx - 6, cy + 6, 12, 6, 2, RGB(130, 155, 195)); cv.fillRect(cx - 5, cy + 8, 2, 2, RGB(255, 40, 40)); cv.fillRect(cx + 3, cy + 8, 2, 2, RGB(255, 40, 40)); break;
    case GM_HORROR:
      cv.fillCircle(cx, cy, 15, RGB(20, 6, 10));
      cv.fillCircle(cx - 6, cy - 3, 3, (millis() / 700 & 1) ? RGB(255, 30, 30) : RGB(90, 10, 10)); cv.fillCircle(cx + 6, cy - 3, 3, (millis() / 700 & 1) ? RGB(255, 30, 30) : RGB(90, 10, 10));
      for (int k = 0; k < 5; k++) cv.fillTriangle(cx - 8 + k * 4, cy + 5, cx - 5 + k * 4, cy + 5, cx - 6 + k * 4, cy + 10, RGB(230, 225, 205)); break;
    case GM_AST:
      for (int k = 0; k < 9; k++) { float a = t * 0.4f + k * 2 * PI / 9, b2 = t * 0.4f + (k + 1) * 2 * PI / 9; float r1 = 11 + (k % 3) * 2, r2 = 11 + ((k + 1) % 3) * 2;
        cv.drawLine(cx - 6 + (int)(cosf(a) * r1), cy - 2 + (int)(sinf(a) * r1), cx - 6 + (int)(cosf(b2) * r2), cy - 2 + (int)(sinf(b2) * r2), c); }
      cv.drawTriangle(cx + 12, cy + 12, cx + 20, cy + 12, cx + 16, cy + 3, RGB(255, 255, 255)); break;
    case GM_CRAFT:
      cv.fillTriangle(cx, cy - 14, cx + 15, cy - 6, cx, cy + 2, RGB(80, 190, 70)); cv.fillTriangle(cx, cy - 14, cx - 15, cy - 6, cx, cy + 2, RGB(100, 210, 85));
      cv.fillTriangle(cx - 15, cy - 6, cx, cy + 2, cx, cy + 16, RGB(120, 84, 54)); cv.fillTriangle(cx - 15, cy - 6, cx - 15, cy + 8, cx, cy + 16, RGB(120, 84, 54));
      cv.fillTriangle(cx + 15, cy - 6, cx, cy + 2, cx, cy + 16, RGB(90, 62, 40)); cv.fillTriangle(cx + 15, cy - 6, cx + 15, cy + 8, cx, cy + 16, RGB(90, 62, 40)); break;
    case GM_HIDE:
      cv.fillRect(cx - 10, cy - 15, 20, 30, RGB(120, 80, 45)); cv.drawRect(cx - 10, cy - 15, 20, 30, RGB(70, 45, 25)); cv.fillRect(cx - 7, cy - 5, 14, 5, C_K);
      if ((millis() / 600) & 1) { cv.fillRect(cx - 5, cy - 4, 2, 2, RGB(255, 240, 200)); cv.fillRect(cx + 3, cy - 4, 2, 2, RGB(255, 240, 200)); } break;
    case GM_ANOM:
      cv.fillTriangle(cx, cy - 10, cx - 22, cy + 14, cx + 22, cy + 14, RGB(34, 38, 36)); cv.drawRect(cx - 5, cy - 4, 10, 10, RGB(80, 90, 85));
      txt("?", cx - 6, cy - 8, ((millis() / 400) & 1) ? RGB(255, 80, 80) : C_W, 2); break;
    case GM_FOREST:
      for (int k = 0; k < 3; k++) { cv.fillTriangle(cx - 16 + k * 3, cy + 8 - k * 7, cx - 6 + k * 3, cy + 8 - k * 7, cx - 11 + k * 3, cy - 6 - k * 7 + 7, RGB(30, 90, 50)); }
      cv.fillRect(cx - 12, cy + 8, 3, 6, RGB(80, 55, 30));
      cv.fillRect(cx + 12, cy - 6, 3, 22, C_K); cv.fillCircle(cx + 13, cy - 9, 3, RGB(235, 235, 230)); break;
    default:
      cv.fillCircle(cx, cy, 10, c);
      for (int k = 0; k < 8; k++) { float a = k * PI / 4 + t * 0.4f; cv.fillCircle(cx + (int)(cosf(a) * 12), cy + (int)(sinf(a) * 12), 3, c); }
      cv.fillCircle(cx, cy, 4, RGB(10, 12, 30)); break;
  }
}
uint16_t menuCol(int i) { return i < NG ? G[i].col : RGB(170, 180, 220); }
float menuX = 0;
void menuFrame(float dt) {
  if (rpt[K_LF] && sel > 0) { sel--; SFX(SFX_MOVE); }
  if (rpt[K_RT] && sel < NM - 1) { sel++; SFX(SFX_MOVE); }
  if (prs[K_B]) { cfg.music = !cfg.music; touchCfg(); if (cfg.music) SFX(SFX_OK); }
  if (prs[K_A] || prs[K_ST]) {
    SFX(SFX_OK);
    if (sel == NG) { state = S_SET; spage = SP_LIST; setSel = 0; setTop = 0; confItem = -1; } else startGame(sel);
    return;
  }
  menuX += (sel - menuX) * fminf(1, dt * 12);
  vgrad(0, SH, RGB(8, 6, 30), RGB(44, 10, 62)); starsDraw(0.6f);
  txtRainbow("NEON ARCADE", 3, 1, millis() / 12);
  char b[24]; snprintf(b, 24, "%d%%", (int)batPct); txt(b, SW - 3 - strlen(b) * 6, 14, noBat ? RGB(140, 140, 200) : (batPct < 20 ? RGB(255, 80, 80) : RGB(120, 255, 140)));
  for (int i = 0; i < NM; i++) {
    float d = i - menuX, ad = fabsf(d); if (ad > 1.7f) continue;
    int cx = 64 + (int)(d * 84), cy = 60; float sc = 1.0f - fminf(1, ad) * 0.2f;
    int w = (int)(76 * sc), h = (int)(72 * sc), x = cx - w / 2, y = cy - h / 2; uint16_t col = menuCol(i);
    cv.fillRoundRect(x, y, w, h, 8, mix(RGB(10, 12, 30), col, 38));
    cv.drawRoundRect(x, y, w, h, 8, mix(col, C_K, (uint8_t)fminf(200, ad * 220)));
    if (ad < 0.5f) cv.drawRoundRect(x - 1, y - 1, w + 2, h + 2, 9, mix(col, C_K, 90 + (int)(sinf(millis() * 0.008f) * 50)));
    icon(i, cx, cy - 10, col);
    if (ad < 0.5f) {
      const char *nm = i < NG ? G[i].name : "SETTINGS"; txtS(nm, cx - (int)strlen(nm) * 3, cy + 14, C_W);
      if (i < NG) snprintf(b, 24, "HI %d", hiS[i]); else snprintf(b, 24, "CUSTOMIZE");
      txt(b, cx - (int)strlen(b) * 3, cy + 25, mix(col, C_W, 100));
    }
  }
  for (int i = 0; i < NM; i++) cv.fillCircle(64 + (i - NM / 2) * 8, 102, i == sel ? 2 : 1, i == sel ? menuCol(sel) : RGB(80, 80, 120));
  txtC("<  >    A:OK", 108, RGB(200, 200, 255)); txtC(cfg.music ? "B: MUSIC ON" : "B: MUSIC OFF", 117, RGB(120, 120, 170));
}
void drawHud() {
  cv.fillRect(0, 0, SW, 11, RGB(8, 10, 26)); cv.drawFastHLine(0, 11, SW, G[cur].col);
  if (cfg.hud == 2) return;
  char b[24]; snprintf(b, 24, "SC %d", score); txt(b, 3, 2, C_W);
  if (cfg.hud == 0) for (int i = 0; i < lives; i++) heart(SW - 10 - i * 9, 1, RGB(255, 60, 90));
}
void panel(int y, int h) { cv.fillRoundRect(14, y, 100, h, 6, RGB(10, 12, 30)); cv.drawRoundRect(14, y, 100, h, 6, G[cur].col); }
void playFrame(float dt) {
  bool np = noPause; noPause = false;
  if (prs[K_ST] && !over && !np) paused = !paused;
  if (!paused && !over) G[cur].upd(dt);
  updPts(dt);
  G[cur].drw(); drawPts(); if (G[cur].hud) drawHud();
  if (paused) { dim(); panel(46, 36); txtC("PAUSED", 54, C_W, 2); txtC("START: RESUME", 72, RGB(180, 180, 255)); }
  if (over) {
    dim(); panel(34, 62);
    if (won) txtC("COMPLETE!", 40, RGB(80, 255, 140), 2); else txtC("GAME OVER", 40, RGB(255, 80, 100), 2);
    char b[24]; snprintf(b, 24, cur == GM_DASH ? "DONE %d%%" : "SCORE %d", score); txtC(b, 60, C_W); snprintf(b, 24, "BEST  %d", hiS[cur]); txtC(b, 70, RGB(255, 220, 80));
    if (newBest && (millis() / 300 & 1)) txtC("NEW BEST!", 80, RGB(80, 255, 140));
    txtC("A:RETRY SEL:MENU", 104, RGB(180, 180, 255));
    if (prs[K_A] || prs[K_ST]) startGame(cur);
  }
}
void sleepOverlay() {
  uint32_t lim = SLEEP_MIN[cfg.sleepIdx] * 60000UL; if (!lim) return;
  uint32_t idle = millis() - lastAct;
  if (idle >= lim) { goSleep(); return; }
  if (lim - idle < 10000) {
    dim(); cv.fillRoundRect(14, 50, 100, 30, 6, RGB(10, 12, 30)); cv.drawRoundRect(14, 50, 100, 30, 6, C_W);
    char b[24]; snprintf(b, 24, "SLEEP IN %d", (int)((lim - idle) / 1000) + 1);
    txtC(b, 56, RGB(255, 220, 80)); txtC("PRESS ANY KEY", 67, RGB(180, 180, 255));
  }
}

// ======================= setup / loop =======================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // منع ريستارت هبوط الجهد لحظيًا
  disableCore0WDT(); disableLoopWDT();
  bool woke = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
  gpio_deep_sleep_hold_dis(); gpio_hold_dis((gpio_num_t)TFT_BL); gpio_hold_dis((gpio_num_t)BUZZER_PIN);
  pref.begin("neonarc", false);
  spkMode = pref.getUChar("spk", 1); sens = pref.getUChar("sens", 3); if (sens < 1 || sens > 5) sens = 3;
  bool known = loadCfg();
  if (woke) for (int k = 0; k < 3; k++) rtc_gpio_deinit((gpio_num_t)PIN_BTN[k]);
  setCpuFrequencyMhz(CPU_MHZ[cfg.cpuIdx]);
  BL_INIT(); BL_WRITE(0);
  audioInit();
  analogReadResolution(12); analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  for (int i = 0; i < 8; i++) pinMode(PIN_BTN[i], INPUT_PULLUP);
  bool mapReset = false;
  if (!woke && digitalRead(PIN_BTN[6]) == LOW && digitalRead(PIN_BTN[7]) == LOW) { for (int i = 0; i < 8; i++) cfg.map[i] = i; saveCfgNow(); mapReset = true; }

  cv.setTextWrap(false); FB = cv.getBuffer();
  coneInit();
  spiOLED.begin(OLED_CLK, -1, OLED_MOSI, OLED_CS);
  if (oled.begin(SSD1306_SWITCHCAPVCC)) { oled.clearDisplay(); oled.display(); }
  spiTFT.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_144GREENTAB);
  tft.setSPISpeed(SPI_MHZ[cfg.spiIdx] * 1000000UL);
  tft.setOffset(cfg.colS, cfg.rowS); tft.setRotation(0); clearGRAM();

  for (int i = 0; i < 16; i++) { char k[4]; snprintf(k, 4, "h%d", i); hiS[i] = pref.getUInt(k, 0); }
  starsInit();
  for (int i = 0; i < 3; i++) readSensors();
  if (!known && batPinV > 0.2f) { cfg.vfac = 4.02f / batPinV; batV = 4.02f; batInit = false; saveCfgNow(); readSensors(); }

  semGo = xSemaphoreCreateBinary(); semDone = xSemaphoreCreateBinary(); xSemaphoreGive(semDone);
  semJ = xSemaphoreCreateBinary(); semJD = xSemaphoreCreateBinary();
  xTaskCreatePinnedToCore(jobTask, "job", 8192, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(tftTask, "tft", 6144, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(infoTask, "info", 8192, NULL, 1, NULL, 0);

  SFX(SFX_BOOT);
  musicOn = true;
  int frames = woke ? 25 : 55;
  for (int f = 0; f < frames; f++) {
    gdt = 0.016f; vgrad(0, SH, RGB(6, 4, 24), RGB(50, 10, 70)); starsDraw(2.0f);
    txtRainbow("NEON", 28, 3, f * 6); txtRainbow("ARCADE", 58, 3, f * 6 + 60);
    if (mapReset) txtC("KEYMAP RESET", 92, RGB(255, 220, 80));
    cv.drawRoundRect(14, 106, 100, 8, 3, RGB(120, 120, 200)); cv.fillRoundRect(16, 108, f * 96 / (frames - 1), 4, 2, wheel(f * 4));
    present();
    int p = min((int)cfg.bl, 10 + f * 6); BL_WRITE((p * p * 255) / 10000);
    delay(16);
  }
  applyBL();
  uint32_t w0 = millis(); while (millis() - w0 < 1500) { bool any = false; for (int i = 0; i < 8; i++) if (digitalRead(PIN_BTN[i]) == LOW) any = true; if (!any) break; delay(10); }
  lastAct = millis();
}

void loop() {
  static uint32_t tLast = 0, tFps = 0; static int fc = 0; static bool prevDim = false;
  uint32_t t0 = micros(); float dt = (t0 - tLast) / 1e6f; tLast = t0; if (dt > 0.05f) dt = 0.05f; gdt = dt;
  readInput(dt);
  musicOn = (state == S_MENU);
  idleDim = (millis() - lastAct > 30000);
  bool wd = idleDim || thermalDim; if (wd != prevDim) { prevDim = wd; applyBL(); }
  if (reqSleep) goSleep();
  if (cfgDirty && millis() - cfgT > 1500 && state != S_PLAY) saveCfgNow();
  if (shake > 0) shake -= dt;

  if (state == S_MENU) menuFrame(dt);
  else if (state == S_SET) settingsFrame(dt);
  else if (prs[K_SE]) { onLeave(); state = S_MENU; sel = cur; shake = 0; audioFlush(); menuFrame(dt); }
  else playFrame(dt);

  sleepOverlay();
  present();
  fc++; if (millis() - tFps >= 1000) { fps = fc * 1000.0f / (millis() - tFps); fc = 0; tFps = millis(); }
  uint32_t el = micros() - t0; if (el < 16000) delayMicroseconds(16000 - el);   // 60 FPS
}
