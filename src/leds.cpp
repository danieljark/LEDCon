#include "leds.h"
#include "config.h"
#include <NeoPixelBus.h>
#include <math.h>
#include "driver/gpio.h"

// Dynamic RMT channel (NeoEsp32RmtChannelN) — same as WLED Gledopto build
// Constructor: NeoPixelBus(count, pin, NeoBusChannel)
typedef NeoPixelBus<NeoGrbFeature, NeoEsp32RmtNWs2812xMethod> LedBus;
static LedBus* _bus = nullptr;
static uint16_t _ledCount = 50;

// ── Global state ─────────────────────────────────────────────────────────────
SegState g_segs[LED_MAX_SEGS];
bool     g_globalEn  = true;
uint8_t  g_globalBri = 255;
uint8_t  g_numSegs   = 4;

SegState g_pSegs[LED_MAX_SEGS];
bool     g_pGlobalEn  = true;
uint8_t  g_pGlobalBri = 255;
volatile bool g_pending = false;
portMUX_TYPE  g_ledMux  = portMUX_INITIALIZER_UNLOCKED;

BootPhase g_bootPhase = BOOT_NO_NET;
static uint32_t _netUpMs = 0;
static bool _relayOn = true;

// Art-Net direct pixel buffer (written by artnet.cpp, rendered by leds_loop)
static RgbColor _artBuf[LED_MAX_COUNT];
static volatile bool _artDirty = false;

// ── Effect math ──────────────────────────────────────────────────────────────

static float breathe(uint32_t ms, uint32_t period) {
    return (1.0f - cosf((float)(ms % period) / period * 2.0f * (float)M_PI)) * 0.5f;
}

static float effectFactor(uint8_t fx, uint32_t ms) {
    switch (fx) {
        case FX_OFF:        return 0.0f;
        case FX_STATIC:     return 1.0f;
        case FX_FLASH_FAST: return (ms % 250u  < 125u)  ? 1.0f : 0.0f;
        case FX_FLASH_SLOW: return (ms % 2000u < 1000u) ? 1.0f : 0.0f;
        case FX_PULSE_FAST: return breathe(ms,  500u);
        case FX_PULSE_SLOW: return breathe(ms, 3000u);
        case FX_STROBE:     return (ms % 50u < 3u) ? 1.0f : 0.0f;
        case FX_PULSE_1HZ:  return breathe(ms, 1000u);
        case FX_CHASE:      return 1.0f;
        default:            return 1.0f;
    }
}

// ── Rendering ────────────────────────────────────────────────────────────────

static void paintSeg(const SegState& seg, uint16_t ledStart, uint16_t ledEnd,
                     uint32_t ms, float gBri)
{
    if (!_bus || ledStart >= _ledCount) return;
    uint16_t end = min(ledEnd, (uint16_t)(_ledCount - 1));

    if (seg.fx == FX_CHASE) {
        uint16_t num  = end - ledStart + 1;
        uint16_t head = (uint16_t)((float)(ms % 1500u) / 1500.0f * num);
        float    sBri = (seg.bri / 255.0f) * gBri;
        for (uint16_t i = 0; i < num; i++) {
            int16_t dist = (int16_t)head - (int16_t)i;
            float   b    = (dist >= 0 && dist < 3) ? (3 - dist) / 3.0f * sBri : 0.0f;
            _bus->SetPixelColor(ledStart + i, RgbColor(
                (uint8_t)(seg.r * b),
                (uint8_t)(seg.g * b),
                (uint8_t)(seg.b * b)
            ));
        }
        return;
    }

    float factor = effectFactor(seg.fx, ms);
    float bri    = factor * (seg.bri / 255.0f) * gBri;
    RgbColor col((uint8_t)(seg.r * bri), (uint8_t)(seg.g * bri), (uint8_t)(seg.b * bri));
    for (uint16_t i = ledStart; i <= end; i++) _bus->SetPixelColor(i, col);
}

// ── Public API ────────────────────────────────────────────────────────────────

void leds_setRelay(bool on) {
    _relayOn = on;
    gpio_set_level(GPIO_NUM_18, on ? 1 : 0);
    Serial.printf("[LED] Relay %s\n", on ? "ON" : "OFF");
}

bool leds_getRelay() { return _relayOn; }

void leds_onNetUp() {
    if (g_bootPhase == BOOT_NO_NET) {
        g_bootPhase = BOOT_NET_PULSE;
        _netUpMs = millis();
        Serial.println("[LED] Boot phase: NET_PULSE (10s blue)");
    }
}

void leds_begin() {
    _ledCount  = g_cfg.strip.count;
    _relayOn   = g_cfg.net.relayOn;
    g_bootPhase = BOOT_NO_NET;

    // GPIO18 = Relay — controls LED strip power supply
    gpio_pad_select_gpio(GPIO_NUM_18);
    gpio_set_direction(GPIO_NUM_18, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_18, _relayOn ? 1 : 0);

    // GPIO5 = ETH PHY power
    gpio_pad_select_gpio(GPIO_NUM_5);
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_5, 1);
    delay(50);

    gpio_reset_pin((gpio_num_t)LED_DATA_PIN);
    _bus = new LedBus(_ledCount, LED_DATA_PIN, NeoBusChannel_0);
    if (_bus) {
        _bus->Begin();
        _bus->ClearTo(RgbColor(0));
        _bus->Show();
    } else {
        Serial.println("[LED] ERROR: Bus alloc failed!");
    }

    g_numSegs   = g_cfg.numSegs;
    g_globalEn  = true;
    g_globalBri = g_cfg.strip.briMax;
    memset(g_segs,  0, sizeof(g_segs));
    memset(g_pSegs, 0, sizeof(g_pSegs));

    Serial.printf("[LED] Ready: pin=%d count=%d relay=%s\n",
                  LED_DATA_PIN, _ledCount, _relayOn ? "ON" : "OFF");
}

void leds_applyConfig() {
    _ledCount = g_cfg.strip.count;
    if (_bus) {
        _bus->ClearTo(RgbColor(0));
        _bus->Show();
    }
    g_numSegs = g_cfg.numSegs;
}

void leds_loop() {
    if (!_bus) return;

    uint32_t ms = millis();

    // ── Boot animation (overrides all segment state) ──────────────────────────
    if (g_bootPhase == BOOT_NO_NET) {
        // Orange fast blink = no network
        bool on = (ms % 500u) < 250u;
        RgbColor col = on ? RgbColor(200, 80, 0) : RgbColor(0);
        for (uint16_t i = 0; i < _ledCount; i++) _bus->SetPixelColor(i, col);
        _bus->Show();
        return;
    }
    if (g_bootPhase == BOOT_NET_PULSE) {
        uint32_t elapsed = ms - _netUpMs;
        if (elapsed >= 10000u) {
            // 10s passed — clear all, reset state, hand over to normal control
            g_bootPhase = BOOT_DONE;
            memset(g_segs,  0, sizeof(g_segs));
            memset(g_pSegs, 0, sizeof(g_pSegs));
            memset(_artBuf, 0, sizeof(_artBuf));
            _artDirty = false;
            g_pending = false;
            _bus->ClearTo(RgbColor(0));
            _bus->Show();
            Serial.println("[LED] Boot done — Modbus/UI control only");
            return;
        }
        // Blue slow pulse (1Hz breathing)
        float t  = (float)(elapsed % 2000u) / 2000.0f;
        float bri = (1.0f - cosf(t * 2.0f * (float)M_PI)) * 0.5f;
        uint8_t b = (uint8_t)(180.0f * bri);
        RgbColor col(0, 20, b);
        for (uint16_t i = 0; i < _ledCount; i++) _bus->SetPixelColor(i, col);
        _bus->Show();
        return;
    }

    // ── Art-Net mode ──────────────────────────────────────────────────────────
    if (g_cfg.mode == MODE_ARTNET) {
        if (_artDirty) {
            for (uint16_t i = 0; i < _ledCount; i++) _bus->SetPixelColor(i, _artBuf[i]);
            _bus->Show();
            _artDirty = false;
        }
        return;
    }

    // ── Normal operation (BOOT_DONE) ──────────────────────────────────────────
    if (g_pending) {
        portENTER_CRITICAL(&g_ledMux);
        memcpy(g_segs, g_pSegs, sizeof(g_segs));
        g_globalEn  = g_pGlobalEn;
        g_globalBri = g_pGlobalBri;
        g_pending   = false;
        portEXIT_CRITICAL(&g_ledMux);
    }

    float gBri = g_globalEn ? (g_globalBri / 255.0f) : 0.0f;
    _bus->ClearTo(RgbColor(0));
    for (uint8_t s = 0; s < g_numSegs && s < LED_MAX_SEGS; s++) {
        if (!g_segs[s].en) continue;
        paintSeg(g_segs[s], g_cfg.segs[s].start, g_cfg.segs[s].end, ms, gBri);
    }
    _bus->Show();
}

// ── Thread-safe accessors ─────────────────────────────────────────────────────

void leds_writeSeg(uint8_t seg, const SegState& s) {
    if (seg >= LED_MAX_SEGS) return;
    portENTER_CRITICAL(&g_ledMux);
    g_pSegs[seg] = s;
    g_pending    = true;
    portEXIT_CRITICAL(&g_ledMux);
}

void leds_writeGlobal(bool en, uint8_t bri) {
    portENTER_CRITICAL(&g_ledMux);
    g_pGlobalEn  = en;
    g_pGlobalBri = bri;
    g_pending    = true;
    portEXIT_CRITICAL(&g_ledMux);
}


SegState leds_readSeg(uint8_t seg) {
    if (seg >= LED_MAX_SEGS) return SegState{};
    portENTER_CRITICAL(&g_ledMux);
    SegState s = g_pSegs[seg];
    portEXIT_CRITICAL(&g_ledMux);
    return s;
}

void leds_writeArtNetGroup(uint16_t start, uint16_t end, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t lim = min(end, (uint16_t)(LED_MAX_COUNT - 1));
    for (uint16_t i = start; i <= lim; i++) _artBuf[i] = RgbColor(r, g, b);
}

void leds_flushArtNet() {
    _artDirty = true;
}
