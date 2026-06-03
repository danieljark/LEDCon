#pragma once
#include <Arduino.h>

#ifndef LED_MAX_SEGS
#define LED_MAX_SEGS 8
#endif

enum ControlMode : uint8_t {
    MODE_WEBAPI = 0,
    MODE_ARTNET = 1,
    MODE_MODBUS = 2
};

struct StripCfg {
    uint8_t  pin     = LED_DATA_PIN;
    char     type[12]= "WS2812B";
    uint16_t count   = LED_DEFAULT_COUNT;
    char     order[5]= "GRB";
    uint8_t  briMax  = 200;
};

struct SegCfg {
    uint16_t start = 0;
    uint16_t end   = 0;
};

struct NetCfg {
    bool     ethDhcp     = false;
    char     ethIp[16]   = "192.168.10.10";
    char     ethMask[16] = "255.255.255.0";
    char     ethGw[16]   = "192.168.10.1";
    char     wifiSsid[33]= "";
    char     wifiPass[65]= "";
    char     apSsid[33]  = "LEDcon";
    bool     apEnabled   = true;
    uint16_t modbusPort  = 502;
    bool     relayOn     = true;   // GPIO18 relay — enables LED strip power
};

struct ArtNetCfg {
    uint16_t universe  = 0;
    uint8_t  groupSize = 3;   // LEDs per group / segment
};

struct Config {
    NetCfg     net;
    StripCfg   strip;
    SegCfg     segs[LED_MAX_SEGS];
    uint8_t    numSegs  = 4;
    uint8_t    mode     = MODE_MODBUS;
    ArtNetCfg  artnet;
};

extern Config g_cfg;

void config_defaults();
bool config_load();
bool config_save(bool force = false);
