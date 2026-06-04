#pragma once
#include <Arduino.h>

#ifndef LED_MAX_SEGS
#define LED_MAX_SEGS 8
#endif
#ifndef LED_MAX_COUNT
#define LED_MAX_COUNT 300
#endif

enum Effect : uint8_t {
    FX_OFF        = 0,
    FX_STATIC     = 1,
    FX_FLASH_FAST = 2,   // 4 Hz
    FX_FLASH_SLOW = 3,   // 0.5 Hz
    FX_PULSE_FAST = 4,   // 2 Hz breathing
    FX_PULSE_SLOW = 5,   // 0.33 Hz breathing
    FX_STROBE     = 6,   // 20 Hz
    FX_PULSE_1HZ  = 7,   // 1 Hz breathing
    FX_CHASE      = 8,   // 3-LED window
};

struct SegState {
    uint8_t r   = 0;
    uint8_t g   = 0;
    uint8_t b   = 0;
    uint8_t bri = 255;
    uint8_t fx  = FX_STATIC;
    bool    en  = false;
};

// Current state (main-loop only)
extern SegState g_segs[LED_MAX_SEGS];
extern bool     g_globalEn;
extern uint8_t  g_globalBri;
extern uint8_t  g_numSegs;

// Pending state (written by Modbus/Web callbacks, read by main loop)
extern SegState g_pSegs[LED_MAX_SEGS];
extern bool     g_pGlobalEn;
extern uint8_t  g_pGlobalBri;
extern volatile bool g_pending;
extern portMUX_TYPE  g_ledMux;

// Boot phases: blink until net up → blue pulse 10s → done (Modbus/UI only)
enum BootPhase : uint8_t { BOOT_NO_NET, BOOT_NET_PULSE, BOOT_DONE };
extern BootPhase g_bootPhase;

void leds_begin();
void leds_loop();
void leds_applyConfig();
void leds_onNetUp();
void leds_setRelay(bool on);
bool leds_getRelay();

// Art-Net direct pixel control (called from artnet.cpp)
void leds_clearArtNetBuffer(uint16_t start, uint16_t end);
void leds_writeArtNetGroup(uint16_t start, uint16_t end, uint8_t r, uint8_t g, uint8_t b);
void leds_flushArtNet();
void leds_artnetPulse();

// Thread-safe write from Modbus / Web task
void leds_writeSeg(uint8_t seg, const SegState& s);
void leds_writeGlobal(bool en, uint8_t bri);
SegState leds_readSeg(uint8_t seg);
