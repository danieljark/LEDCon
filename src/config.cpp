#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>

Config g_cfg;

static uint32_t _lastSave = 0;
static const char* CONFIG_PATH = "/config.json";
static const char* CONFIG_TMP_PATH = "/config.tmp";
static const char* CONFIG_BAK_PATH = "/config.bak";

bool config_hashPassword(const char* password, char outHash[65]) {
    if (!password || !outHash) return false;

    uint8_t hash[32];
    mbedtls_sha256((const unsigned char*)password, strlen(password), hash, 0);
    for (uint8_t i = 0; i < sizeof(hash); i++) {
        sprintf(outHash + (i * 2), "%02x", hash[i]);
    }
    outHash[64] = '\0';
    return true;
}

bool config_authConfigured() {
    return g_cfg.auth.passwordHash[0] != '\0';
}

static void clampConfigToHardware() {
    if (g_cfg.numSegs > LED_MAX_SEGS) {
        g_cfg.numSegs = LED_MAX_SEGS;
    }

    if (g_cfg.strip.count < 1) {
        g_cfg.strip.count = 1;
    }
    if (g_cfg.strip.count > LED_MAX_COUNT) {
        Serial.printf("[CFG] strip.count %u > LED_MAX_COUNT %u, clamping\n",
                      g_cfg.strip.count, LED_MAX_COUNT);
        g_cfg.strip.count = LED_MAX_COUNT;
    }

    bool usedLeds[LED_MAX_COUNT] = {};
    SegCfg validSegs[LED_MAX_SEGS];
    uint8_t validCount = 0;
    uint16_t maxLed = (g_cfg.strip.count > 0) ? (g_cfg.strip.count - 1) : 0;
    for (uint8_t i = 0; i < g_cfg.numSegs; i++) {
        if (g_cfg.segs[i].start > maxLed) g_cfg.segs[i].start = maxLed;
        if (g_cfg.segs[i].end   > maxLed) g_cfg.segs[i].end   = maxLed;
        if (g_cfg.segs[i].end < g_cfg.segs[i].start) {
            uint16_t tmp = g_cfg.segs[i].start;
            g_cfg.segs[i].start = g_cfg.segs[i].end;
            g_cfg.segs[i].end = tmp;
        }

        bool overlaps = false;
        for (uint16_t led = g_cfg.segs[i].start; led <= g_cfg.segs[i].end; led++) {
            if (usedLeds[led]) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            Serial.printf("[CFG] Dropping overlapping segment %u\n", i);
            continue;
        }
        for (uint16_t led = g_cfg.segs[i].start; led <= g_cfg.segs[i].end; led++) {
            usedLeds[led] = true;
        }
        validSegs[validCount++] = g_cfg.segs[i];
    }
    memcpy(g_cfg.segs, validSegs, validCount * sizeof(SegCfg));
    g_cfg.numSegs = validCount;

    if (strlen(g_cfg.net.apPass) < 8) {
        strlcpy(g_cfg.net.apPass, "LEDcon-Setup", sizeof(g_cfg.net.apPass));
    }
    if (g_cfg.artnet.groupSize < 1 || g_cfg.artnet.groupSize > ARTNET_MAX_GROUPS) {
        g_cfg.artnet.groupSize = ARTNET_DEFAULT_GROUP_SIZE;
    }
    if (g_cfg.mode > MODE_MODBUS) {
        g_cfg.mode = MODE_MODBUS;
    }
}

void config_defaults() {
    g_cfg = Config{};
    // Default 4 equal segments over 50 LEDs
    g_cfg.numSegs = 4;
    g_cfg.segs[0].start=0;  g_cfg.segs[0].end=11;
    g_cfg.segs[1].start=12; g_cfg.segs[1].end=23;
    g_cfg.segs[2].start=24; g_cfg.segs[2].end=36;
    g_cfg.segs[3].start=37; g_cfg.segs[3].end=49;
    clampConfigToHardware();
}

bool config_load() {
    if (!LittleFS.begin(true)) {
        Serial.println("[CFG] LittleFS mount failed, using defaults");
        config_defaults();
        return false;
    }
    const char* loadPath = CONFIG_PATH;
    if (!LittleFS.exists(loadPath) && LittleFS.exists(CONFIG_BAK_PATH)) {
        loadPath = CONFIG_BAK_PATH;
        Serial.println("[CFG] Primary config missing, loading backup");
    }
    if (!LittleFS.exists(loadPath)) {
        Serial.println("[CFG] No config found, using defaults");
        config_defaults();
        return false;
    }
    File f = LittleFS.open(loadPath, "r");
    if (!f) { config_defaults(); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[CFG] JSON parse error in %s: %s\n", loadPath, err.c_str());
        if (loadPath == CONFIG_PATH && LittleFS.exists(CONFIG_BAK_PATH)) {
            File bf = LittleFS.open(CONFIG_BAK_PATH, "r");
            if (bf) {
                err = deserializeJson(doc, bf);
                bf.close();
                if (!err) {
                    Serial.println("[CFG] Loaded backup config");
                }
            }
        }
        if (err) {
            config_defaults();
            return false;
        }
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
        strlcpy(g_cfg.net.apPass,  net["apPass"]| "LEDcon-Setup",  65);
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
        g_cfg.artnet.universe  = an["univ"]  | ARTNET_DEFAULT_UNIVERSE;
        g_cfg.artnet.groupSize = an["grp"]   | ARTNET_DEFAULT_GROUP_SIZE;
    }

    JsonObject auth = doc["auth"];
    if (!auth.isNull()) {
        strlcpy(g_cfg.auth.username, auth["user"] | "admin", 33);
        strlcpy(g_cfg.auth.passwordHash, auth["hash"] | "", 65);

        const char* legacyPass = auth["pass"] | "";
        if (g_cfg.auth.passwordHash[0] == '\0' && legacyPass[0] != '\0') {
            config_hashPassword(legacyPass, g_cfg.auth.passwordHash);
            Serial.println("[CFG] Migrated auth password to SHA-256 hash");
        }
    }

    clampConfigToHardware();

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
    net["apPass"] = g_cfg.net.apPass;
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
    for (int i = 0; i < g_cfg.numSegs && i < LED_MAX_SEGS; i++) {
        JsonObject s = segs.add<JsonObject>();
        s["s"] = g_cfg.segs[i].start;
        s["e"] = g_cfg.segs[i].end;
    }

    doc["mode"] = g_cfg.mode;
    JsonObject an = doc["artnet"].to<JsonObject>();
    an["univ"] = g_cfg.artnet.universe;
    an["grp"]  = g_cfg.artnet.groupSize;

    JsonObject auth = doc["auth"].to<JsonObject>();
    auth["user"] = g_cfg.auth.username;
    auth["hash"] = g_cfg.auth.passwordHash;

    LittleFS.remove(CONFIG_TMP_PATH);
    File f = LittleFS.open(CONFIG_TMP_PATH, "w");
    if (!f) { Serial.println("[CFG] Save failed"); return false; }
    if (serializeJson(doc, f) == 0) {
        f.close();
        LittleFS.remove(CONFIG_TMP_PATH);
        Serial.println("[CFG] Save failed: JSON write");
        return false;
    }
    f.close();

    LittleFS.remove(CONFIG_BAK_PATH);
    if (LittleFS.exists(CONFIG_PATH) && !LittleFS.rename(CONFIG_PATH, CONFIG_BAK_PATH)) {
        LittleFS.remove(CONFIG_TMP_PATH);
        Serial.println("[CFG] Save failed: backup rename");
        return false;
    }
    if (!LittleFS.rename(CONFIG_TMP_PATH, CONFIG_PATH)) {
        if (LittleFS.exists(CONFIG_BAK_PATH)) LittleFS.rename(CONFIG_BAK_PATH, CONFIG_PATH);
        Serial.println("[CFG] Save failed: final rename");
        return false;
    }
    LittleFS.remove(CONFIG_BAK_PATH);
    Serial.println("[CFG] Saved");
    return true;
}
