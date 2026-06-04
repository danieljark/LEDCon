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

// Art-Net frame buffers. UDP callbacks never write the buffer being rendered.
static RgbColor _artWriteBuf[LED_MAX_COUNT];
static RgbColor _artActiveBuf[LED_MAX_COUNT];
static RgbColor _artRenderBuf[LED_MAX_COUNT];
static volatile bool _artDirty = false;
static volatile bool _configDirty = false;
static uint32_t _artNetLastPacket = 0;
static bool _artNetTimedOut = true;
static const uint32_t ARTNET_TIMEOUT_MS = 10000;
static const uint32_t MODBUS_TIMEOUT_MS = 10000;
static const uint32_t LED_FRAME_INTERVAL_MS = 10;
static uint32_t _modbusLastWrite = 0;
static bool _modbusWatchdogArmed = false;
static bool _modbusTimedOut = false;
static bool _normalRendered = false;

static bool createBus(uint16_t count) {
    if (_bus) {
        delete _bus;
        _bus = nullptr;
    }

    // Pull data line LOW before NeoPixelBus init — prevents WS2812B from
    // latching garbage bits when GPIO floats HIGH during ESP reset
    gpio_pad_select_gpio((gpio_num_t)LED_DATA_PIN);
    gpio_set_direction((gpio_num_t)LED_DATA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LED_DATA_PIN, 0);
    delay(2);
    gpio_reset_pin((gpio_num_t)LED_DATA_PIN);

    _bus = new LedBus(count, LED_DATA_PIN, NeoBusChannel_0);
    if (!_bus) {
        Serial.println("[LED] ERROR: Bus alloc failed!");
        return false;
    }

    _bus->Begin();
    _bus->ClearTo(RgbColor(0));
    _bus->Show();
    return true;
}

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

static bool isAnimatedEffect(uint8_t fx) {
    switch (fx) {
        case FX_FLASH_FAST:
        case FX_FLASH_SLOW:
        case FX_PULSE_FAST:
        case FX_PULSE_SLOW:
        case FX_STROBE:
        case FX_PULSE_1HZ:
        case FX_CHASE:
            return true;
        default:
            return false;
    }
}

// ── Rendering ────────────────────────────────────────────────────────────────

static void paintSeg(const SegState& seg, uint16_t ledStart, uint16_t ledEnd,
                     uint32_t ms, float gBri)
{
    if (!_bus || ledStart >= _ledCount) return;
    if (ledEnd < ledStart) return;
    uint16_t end = min(ledEnd, (uint16_t)(_ledCount - 1));

    if (seg.fx == FX_CHASE) {
        uint16_t num  = end - ledStart + 1;
        if (num == 0) return;
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
    _ledCount  = min(g_cfg.strip.count, (uint16_t)LED_MAX_COUNT);
    g_cfg.strip.count = _ledCount;
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

    createBus(_ledCount);

    g_numSegs   = g_cfg.numSegs;
    g_globalEn  = true;
    g_globalBri = g_cfg.strip.briMax;
    memset(g_segs,  0, sizeof(g_segs));
    memset(g_pSegs, 0, sizeof(g_pSegs));

    Serial.printf("[LED] Ready: pin=%d count=%d relay=%s\n",
                  LED_DATA_PIN, _ledCount, _relayOn ? "ON" : "OFF");
}

void leds_applyConfig() {
    portENTER_CRITICAL(&g_ledMux);
    _configDirty = true;
    portEXIT_CRITICAL(&g_ledMux);
}

void leds_loop() {
    bool applyConfig = false;
    portENTER_CRITICAL(&g_ledMux);
    if (_configDirty) {
        _configDirty = false;
        applyConfig = true;
    }
    portEXIT_CRITICAL(&g_ledMux);

    if (applyConfig) {
        // Production-safe path: the LED bus is created only at boot.
        g_cfg.strip.count = _ledCount;
        memset(_artWriteBuf, 0, sizeof(_artWriteBuf));
        memset(_artActiveBuf, 0, sizeof(_artActiveBuf));
        memset(_artRenderBuf, 0, sizeof(_artRenderBuf));
        _artDirty = false;
        _artNetTimedOut = true;
        g_numSegs = g_cfg.numSegs;
        _normalRendered = false;
    }

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
            memset(_artWriteBuf, 0, sizeof(_artWriteBuf));
            memset(_artActiveBuf, 0, sizeof(_artActiveBuf));
            memset(_artRenderBuf, 0, sizeof(_artRenderBuf));
            _artDirty = false;
            g_pending = false;
            _normalRendered = false;
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
        uint32_t elapsed = ms - _artNetLastPacket;
        if (elapsed > ARTNET_TIMEOUT_MS) {
            if (!_artNetTimedOut) {
                Serial.printf("[LED] ArtNet timeout after %lums — clearing\n", elapsed);
                portENTER_CRITICAL(&g_ledMux);
                memset(_artWriteBuf, 0, sizeof(_artWriteBuf));
                memset(_artActiveBuf, 0, sizeof(_artActiveBuf));
                memset(_artRenderBuf, 0, sizeof(_artRenderBuf));
                portEXIT_CRITICAL(&g_ledMux);
                _bus->ClearTo(RgbColor(0));
                _bus->Show();
                _artDirty = false;
                _artNetTimedOut = true;
            }
            return;
        }
        if (_artDirty) {
            portENTER_CRITICAL(&g_ledMux);
            memcpy(_artRenderBuf, _artActiveBuf, sizeof(_artRenderBuf));
            _artDirty = false;
            portEXIT_CRITICAL(&g_ledMux);
            for (uint16_t i = 0; i < _ledCount; i++) _bus->SetPixelColor(i, _artRenderBuf[i]);
            _bus->Show();
        }
        return;
    }

    // ── Normal operation (BOOT_DONE) ──────────────────────────────────────────
    static uint32_t _lastFrameMs = 0;
    if (ms - _lastFrameMs < LED_FRAME_INTERVAL_MS) return;
    _lastFrameMs = ms;

    if (_modbusWatchdogArmed && !_modbusTimedOut && (ms - _modbusLastWrite > MODBUS_TIMEOUT_MS)) {
        portENTER_CRITICAL(&g_ledMux);
        memset(g_pSegs, 0, sizeof(g_pSegs));
        g_pGlobalEn = false;
        g_pending = true;
        portEXIT_CRITICAL(&g_ledMux);
        _modbusTimedOut = true;
        Serial.println("[LED] Modbus timeout — all LEDs off");
    }

    if (g_pending) {
        portENTER_CRITICAL(&g_ledMux);
        memcpy(g_segs, g_pSegs, sizeof(g_segs));
        g_globalEn  = g_pGlobalEn;
        g_globalBri = g_pGlobalBri;
        g_pending   = false;
        portEXIT_CRITICAL(&g_ledMux);
        _normalRendered = false;
    }

    bool needsFrame = !_normalRendered;
    if (!needsFrame) {
        for (uint8_t s = 0; s < g_numSegs && s < LED_MAX_SEGS; s++) {
            if (g_segs[s].en && isAnimatedEffect(g_segs[s].fx)) {
                needsFrame = true;
                break;
            }
        }
    }
    if (!needsFrame) return;

    float gBri = g_globalEn ? (g_globalBri / 255.0f) : 0.0f;
    _bus->ClearTo(RgbColor(0));
    for (uint8_t s = 0; s < g_numSegs && s < LED_MAX_SEGS; s++) {
        if (!g_segs[s].en) continue;
        uint16_t start, end;
        portENTER_CRITICAL(&g_ledMux);
        start = g_cfg.segs[s].start;
        end   = g_cfg.segs[s].end;
        portEXIT_CRITICAL(&g_ledMux);
        paintSeg(g_segs[s], start, end, ms, gBri);
    }
    _bus->Show();
    _normalRendered = true;
}

// ── Thread-safe accessors ─────────────────────────────────────────────────────

void leds_writeSeg(uint8_t seg, const SegState& s) {
    leds_patchSeg(seg, SEG_PATCH_ALL, s);
}

bool leds_patchSeg(uint8_t seg, uint8_t mask, const SegState& patch) {
    if (seg >= LED_MAX_SEGS) return false;
    portENTER_CRITICAL(&g_ledMux);
    SegState& s = g_pSegs[seg];
    if (mask & SEG_PATCH_R)   s.r   = patch.r;
    if (mask & SEG_PATCH_G)   s.g   = patch.g;
    if (mask & SEG_PATCH_B)   s.b   = patch.b;
    if (mask & SEG_PATCH_BRI) s.bri = patch.bri;
    if (mask & SEG_PATCH_FX)  s.fx  = patch.fx;
    if (mask & SEG_PATCH_EN)  s.en  = patch.en;
    g_pending    = true;
    portEXIT_CRITICAL(&g_ledMux);
    return true;
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
    if (start >= LED_MAX_COUNT) return;
    uint16_t lim = min(end, (uint16_t)(LED_MAX_COUNT - 1));
    if (start > lim) return;
    portENTER_CRITICAL(&g_ledMux);
    for (uint16_t i = start; i <= lim; i++) _artWriteBuf[i] = RgbColor(r, g, b);
    portEXIT_CRITICAL(&g_ledMux);
}

void leds_clearArtNetBuffer(uint16_t start, uint16_t end) {
    if (start >= _ledCount) return;
    uint16_t lim = min(end, (uint16_t)(_ledCount - 1));
    if (start > lim) return;
    portENTER_CRITICAL(&g_ledMux);
    for (uint16_t i = start; i <= lim; i++) _artWriteBuf[i] = RgbColor(0);
    portEXIT_CRITICAL(&g_ledMux);
}

void leds_flushArtNet() {
    portENTER_CRITICAL(&g_ledMux);
    memcpy(_artActiveBuf, _artWriteBuf, sizeof(_artActiveBuf));
    _artNetLastPacket = millis();
    _artNetTimedOut = false;
    _artDirty = true;
    portEXIT_CRITICAL(&g_ledMux);
}

void leds_artnetPulse() {
    _artNetLastPacket = millis();
    _artNetTimedOut = false;
}

void leds_touchModbus() {
    _modbusLastWrite = millis();
    _modbusWatchdogArmed = true;
    _modbusTimedOut = false;
}
