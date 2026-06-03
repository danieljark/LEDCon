#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

Config g_cfg;

static uint32_t _lastSave = 0;

void config_defaults() {
    g_cfg = Config{};
    // Default 4 equal segments over 50 LEDs
    g_cfg.numSegs = 4;
    g_cfg.segs[0].start=0;  g_cfg.segs[0].end=11;
    g_cfg.segs[1].start=12; g_cfg.segs[1].end=23;
    g_cfg.segs[2].start=24; g_cfg.segs[2].end=36;
    g_cfg.segs[3].start=37; g_cfg.segs[3].end=49;
}

bool config_load() {
    if (!LittleFS.begin(true)) {
        Serial.println("[CFG] LittleFS mount failed, using defaults");
        config_defaults();
        return false;
    }
    if (!LittleFS.exists("/config.json")) {
        Serial.println("[CFG] No config found, using defaults");
        config_defaults();
        return false;
    }
    File f = LittleFS.open("/config.json", "r");
    if (!f) { config_defaults(); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[CFG] JSON parse error: %s\n", err.c_str());
        config_defaults();
        return false;
    }

    JsonObject net = doc["net"];
    if (!net.isNull()) {
        g_cfg.net.ethDhcp = net["dhcp"] | false;
        strlcpy(g_cfg.net.ethIp,   net["ip"]   | "192.168.10.10", 16);
        strlcpy(g_cfg.net.ethMask, net["mask"]  | "255.255.255.0", 16);
        strlcpy(g_cfg.net.ethGw,   net["gw"]    | "192.168.10.1",  16);
        strlcpy(g_cfg.net.wifiSsid,net["ssid"]  | "",              33);
        strlcpy(g_cfg.net.wifiPass,net["pass"]  | "",              65);
        strlcpy(g_cfg.net.apSsid,  net["ap"]    | "LEDcon",        33);
        g_cfg.net.apEnabled  = net["apEn"]   | true;
        g_cfg.net.relayOn    = net["relay"]  | true;
        g_cfg.net.modbusPort = net["mbport"] | 502;
    }

    JsonObject strip = doc["strip"];
    if (!strip.isNull()) {
        g_cfg.strip.pin    = strip["pin"]    | (uint8_t)LED_DATA_PIN;
        g_cfg.strip.count  = strip["count"]  | (uint16_t)LED_DEFAULT_COUNT;
        g_cfg.strip.briMax = strip["briMax"] | (uint8_t)200;
        strlcpy(g_cfg.strip.type,  strip["type"]  | "WS2812B", 12);
        strlcpy(g_cfg.strip.order, strip["order"] | "GRB",      5);
    }

    JsonArray segs = doc["segs"].as<JsonArray>();
    if (!segs.isNull()) {
        g_cfg.numSegs = 0;
        for (JsonObject s : segs) {
            if (g_cfg.numSegs >= LED_MAX_SEGS) break;
            g_cfg.segs[g_cfg.numSegs].start = s["s"] | 0;
            g_cfg.segs[g_cfg.numSegs].end   = s["e"] | 0;
            g_cfg.numSegs++;
        }
    } else {
        config_defaults();
    }

    g_cfg.mode = doc["mode"] | (uint8_t)MODE_MODBUS;
    JsonObject an = doc["artnet"];
    if (!an.isNull()) {
        g_cfg.artnet.universe  = an["univ"]  | 0;
        g_cfg.artnet.groupSize = an["grp"]   | 3;
    }

    Serial.printf("[CFG] Loaded: %d segs, pin=%d, count=%d\n",
                  g_cfg.numSegs, g_cfg.strip.pin, g_cfg.strip.count);
    return true;
}

bool config_save(bool force) {
    uint32_t now = millis();
    if (!force && now - _lastSave < 60000 && _lastSave != 0) return true;
    _lastSave = now;

    JsonDocument doc;
    JsonObject net = doc["net"].to<JsonObject>();
    net["dhcp"]   = g_cfg.net.ethDhcp;
    net["ip"]     = g_cfg.net.ethIp;
    net["mask"]   = g_cfg.net.ethMask;
    net["gw"]     = g_cfg.net.ethGw;
    net["ssid"]   = g_cfg.net.wifiSsid;
    net["pass"]   = g_cfg.net.wifiPass;
    net["ap"]     = g_cfg.net.apSsid;
    net["apEn"]   = g_cfg.net.apEnabled;
    net["relay"]  = g_cfg.net.relayOn;
    net["mbport"] = g_cfg.net.modbusPort;

    JsonObject strip = doc["strip"].to<JsonObject>();
    strip["pin"]    = g_cfg.strip.pin;
    strip["count"]  = g_cfg.strip.count;
    strip["briMax"] = g_cfg.strip.briMax;
    strip["type"]   = g_cfg.strip.type;
    strip["order"]  = g_cfg.strip.order;

    JsonArray segs = doc["segs"].to<JsonArray>();
    for (int i = 0; i < g_cfg.numSegs; i++) {
        JsonObject s = segs.add<JsonObject>();
        s["s"] = g_cfg.segs[i].start;
        s["e"] = g_cfg.segs[i].end;
    }

    doc["mode"] = g_cfg.mode;
    JsonObject an = doc["artnet"].to<JsonObject>();
    an["univ"] = g_cfg.artnet.universe;
    an["grp"]  = g_cfg.artnet.groupSize;

    File f = LittleFS.open("/config.json", "w");
    if (!f) { Serial.println("[CFG] Save failed"); return false; }
    serializeJson(doc, f);
    f.close();
    Serial.println("[CFG] Saved");
    return true;
}
