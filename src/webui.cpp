#include "webui.h"
#include "leds.h"
#include "config.h"
#include "net.h"
#include "artnet.h"
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

static AsyncWebServer _srv(80);

// ── Status cache (reduce JSON allocations every 100ms) ──────────────────────
static String _cachedStatus;
static uint32_t _statusCacheTime = 0;
#define STATUS_CACHE_MS 100

// ── Auth helper ──────────────────────────────────────────────────────────────
static bool constantTimeEqual(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    uint8_t diff = (uint8_t)(la ^ lb);
    size_t n = max(la, lb);
    for (size_t i = 0; i < n; i++) {
        char ca = (i < la) ? a[i] : 0;
        char cb = (i < lb) ? b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

static bool requestHasValidAuth(AsyncWebServerRequest* req) {
    if (!config_authConfigured()) return false;
    if (!req->hasHeader("Authorization")) return false;

    String header = req->header("Authorization");
    if (!header.startsWith("Basic ")) return false;

    String encoded = header.substring(6);
    uint8_t decoded[100];
    size_t decodedLen = 0;
    int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decodedLen,
                                   (const unsigned char*)encoded.c_str(), encoded.length());
    if (rc != 0 || decodedLen == 0 || decodedLen >= sizeof(decoded)) return false;
    decoded[decodedLen] = '\0';

    char* sep = strchr((char*)decoded, ':');
    if (!sep) return false;
    *sep = '\0';
    const char* user = (const char*)decoded;
    const char* pass = sep + 1;

    char hash[65];
    if (!config_hashPassword(pass, hash)) return false;
    return strcmp(user, g_cfg.auth.username) == 0 &&
           constantTimeEqual(hash, g_cfg.auth.passwordHash);
}

static bool requireAuth(AsyncWebServerRequest* req) {
    if (!config_authConfigured()) {
        req->send(403, "text/plain", "Admin password required. Set it via /api/auth first.");
        return false;
    }
    if (!requestHasValidAuth(req)) {
        req->requestAuthentication();
        return false;
    }
    return true;
}

#define REQUIRE_AUTH(req) do { if (!requireAuth(req)) return; } while(0)

// ── /api/status ──────────────────────────────────────────────────────────────
static void handleStatus(AsyncWebServerRequest* req) {
    uint32_t now = millis();
    if (now - _statusCacheTime > STATUS_CACHE_MS) {
        JsonDocument doc;
        doc["ip"]      = net_localIP().toString();
        doc["ap_ip"]   = "192.168.10.1";
        doc["uptime"]  = now / 1000;
        doc["heap"]    = ESP.getFreeHeap();
        doc["version"] = LEDCON_VERSION;
        doc["segs"]    = g_numSegs;
        doc["globalEn"] = g_globalEn;
        doc["globalBri"]= g_globalBri;
        doc["relay"]   = leds_getRelay();
        doc["boot"]    = (int)g_bootPhase;
        doc["mode"]    = (int)g_cfg.mode;
        doc["artnet"]["univ"] = g_cfg.artnet.universe;
        doc["artnet"]["grp"]  = g_cfg.artnet.groupSize;
        doc["artnet"]["groups"] = (g_cfg.strip.count + g_cfg.artnet.groupSize - 1) / g_cfg.artnet.groupSize;
        doc["artnet"]["chTotal"] = ((g_cfg.strip.count + g_cfg.artnet.groupSize - 1) / g_cfg.artnet.groupSize) * 4;
        doc["mbport"]  = g_cfg.net.modbusPort;

        JsonArray segs = doc["seg"].to<JsonArray>();
        for (uint8_t i = 0; i < g_numSegs && i < LED_MAX_SEGS; i++) {
            JsonObject s = segs.add<JsonObject>();
            SegState st  = leds_readSeg(i);
            s["r"]  = st.r; s["g"] = st.g; s["b"] = st.b;
            s["bri"]= st.bri; s["fx"] = st.fx; s["en"] = st.en;
        }
        serializeJson(doc, _cachedStatus);
        _statusCacheTime = now;
    }
    req->send(200, "application/json", _cachedStatus);
}

// ── POST /api/seg/{n} ────────────────────────────────────────────────────────
static void handleSegPost(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                          size_t, size_t) {
    REQUIRE_AUTH(req);

    uint8_t idx = 0;
    if (req->hasParam("n")) idx = (uint8_t)req->getParam("n")->value().toInt();
    if (idx >= LED_MAX_SEGS) { req->send(400, "text/plain", "segment index out of range"); return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf("[WEB] JSON error in /api/seg: %s\n", err.c_str());
        req->send(400, "text/plain", err.c_str());
        return;
    }

    SegState patch{};
    uint8_t mask = 0;
    if (doc["r"].is<int>())   { patch.r   = (uint8_t)constrain(doc["r"].as<int>(),   0, 255); mask |= SEG_PATCH_R; }
    if (doc["g"].is<int>())   { patch.g   = (uint8_t)constrain(doc["g"].as<int>(),   0, 255); mask |= SEG_PATCH_G; }
    if (doc["b"].is<int>())   { patch.b   = (uint8_t)constrain(doc["b"].as<int>(),   0, 255); mask |= SEG_PATCH_B; }
    if (doc["bri"].is<int>()) { patch.bri = (uint8_t)constrain(doc["bri"].as<int>(), 0, 255); mask |= SEG_PATCH_BRI; }
    if (doc["fx"].is<int>())  { patch.fx  = (uint8_t)constrain(doc["fx"].as<int>(),  0, 8);   mask |= SEG_PATCH_FX; }
    if (doc["en"].is<bool>()) { patch.en  = doc["en"].as<bool>();                         mask |= SEG_PATCH_EN; }
    if (mask == 0) { req->send(400, "text/plain", "no segment fields"); return; }
    leds_patchSeg(idx, mask, patch);
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── POST /api/global ─────────────────────────────────────────────────────────
static void handleGlobalPost(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                             size_t, size_t) {
    REQUIRE_AUTH(req);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf("[WEB] JSON error in /api/global: %s\n", err.c_str());
        req->send(400, "text/plain", err.c_str());
        return;
    }
    portENTER_CRITICAL(&g_ledMux);
    bool currentEn = g_pGlobalEn;
    uint8_t currentBri = g_pGlobalBri;
    portEXIT_CRITICAL(&g_ledMux);
    bool    en  = doc["en"]  | currentEn;
    uint8_t bri = doc["bri"].is<int>() ? (uint8_t)constrain(doc["bri"].as<int>(), 0, 255) : currentBri;
    leds_writeGlobal(en, bri);
    req->send(200, "application/json", "{\"ok\":true}");
}

// ── POST /api/net ─────────────────────────────────────────────────────────────
static void handleNetPost(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                          size_t, size_t) {
    REQUIRE_AUTH(req);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf("[WEB] JSON error in /api/net: %s\n", err.c_str());
        req->send(400, "text/plain", err.c_str());
        return;
    }
    g_cfg.net.ethDhcp = doc["dhcp"] | false;
    strlcpy(g_cfg.net.ethIp,    doc["ip"]    | g_cfg.net.ethIp,    16);
    strlcpy(g_cfg.net.ethMask,  doc["mask"]  | g_cfg.net.ethMask,  16);
    strlcpy(g_cfg.net.ethGw,    doc["gw"]    | g_cfg.net.ethGw,    16);
    strlcpy(g_cfg.net.wifiSsid, doc["ssid"]  | g_cfg.net.wifiSsid, 33);
    strlcpy(g_cfg.net.wifiPass, doc["pass"]  | g_cfg.net.wifiPass, 65);
    strlcpy(g_cfg.net.apSsid,   doc["ap"]    | g_cfg.net.apSsid,   33);
    if (doc["apPass"].is<const char*>()) {
        const char* apPass = doc["apPass"].as<const char*>();
        if (strlen(apPass) < 8 || strlen(apPass) > 64) {
            req->send(400, "text/plain", "AP password must be 8-64 chars");
            return;
        }
        strlcpy(g_cfg.net.apPass, apPass, sizeof(g_cfg.net.apPass));
    }
    g_cfg.net.apEnabled  = doc["apEn"]   | g_cfg.net.apEnabled;
    g_cfg.net.modbusPort = doc["mbport"] | g_cfg.net.modbusPort;
    config_save(true);
    req->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    delay(500);
    ESP.restart();
}

// ── POST /api/leds ────────────────────────────────────────────────────────────
static void handleLedsPost(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                           size_t, size_t) {
    REQUIRE_AUTH(req);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (err) {
        Serial.printf("[WEB] JSON error in /api/leds: %s\n", err.c_str());
        req->send(400, "text/plain", err.c_str());
        return;
    }

    uint8_t newPin = g_cfg.strip.pin;
    uint16_t newCount = g_cfg.strip.count;
    uint8_t newBriMax = g_cfg.strip.briMax;
    char newType[sizeof(g_cfg.strip.type)];
    char newOrder[sizeof(g_cfg.strip.order)];
    strlcpy(newType, g_cfg.strip.type, sizeof(newType));
    strlcpy(newOrder, g_cfg.strip.order, sizeof(newOrder));

    JsonObject strip = doc["strip"];
    if (!strip.isNull()) {
        newPin    = strip["pin"]    | newPin;
        newCount  = strip["count"]  | newCount;
        newBriMax = strip["briMax"] | newBriMax;
        strlcpy(newType,  strip["type"]  | newType,  sizeof(newType));
        strlcpy(newOrder, strip["order"] | newOrder, sizeof(newOrder));
        if (newCount < 1) newCount = 1;
        if (newCount > LED_MAX_COUNT) newCount = LED_MAX_COUNT;
    }

    SegCfg newSegs[LED_MAX_SEGS];
    memcpy(newSegs, g_cfg.segs, sizeof(newSegs));
    uint8_t newNumSegs = g_cfg.numSegs;
    bool usedLeds[LED_MAX_COUNT];
    memset(usedLeds, 0, sizeof(usedLeds));

    JsonArray segs = doc["segs"].as<JsonArray>();
    if (!segs.isNull()) {
        newNumSegs = 0;
        uint16_t maxLed = (newCount > 0) ? (newCount - 1) : 0;
        for (JsonObject s : segs) {
            if (newNumSegs >= LED_MAX_SEGS) break;
            uint16_t start = s["s"] | 0;
            uint16_t end   = s["e"] | 0;
            if (start > maxLed) start = maxLed;
            if (end   > maxLed) end   = maxLed;
            if (end < start) {
                uint16_t tmp = start;
                start = end;
                end = tmp;
            }
            for (uint16_t led = start; led <= end; led++) {
                if (usedLeds[led]) {
                    req->send(400, "text/plain", "segments must not overlap");
                    return;
                }
                usedLeds[led] = true;
            }
            newSegs[newNumSegs].start = start;
            newSegs[newNumSegs].end   = end;
            newNumSegs++;
        }
    } else {
        uint16_t maxLed = (newCount > 0) ? (newCount - 1) : 0;
        for (uint8_t i = 0; i < newNumSegs && i < LED_MAX_SEGS; i++) {
            if (newSegs[i].start > maxLed) newSegs[i].start = maxLed;
            if (newSegs[i].end > maxLed) newSegs[i].end = maxLed;
            if (newSegs[i].end < newSegs[i].start) {
                uint16_t tmp = newSegs[i].start;
                newSegs[i].start = newSegs[i].end;
                newSegs[i].end = tmp;
            }
            for (uint16_t led = newSegs[i].start; led <= newSegs[i].end; led++) {
                if (usedLeds[led]) {
                    req->send(400, "text/plain", "segments must not overlap");
                    return;
                }
                usedLeds[led] = true;
            }
        }
    }

    portENTER_CRITICAL(&g_ledMux);
    g_cfg.strip.pin = newPin;
    g_cfg.strip.count = newCount;
    g_cfg.strip.briMax = newBriMax;
    strlcpy(g_cfg.strip.type, newType, sizeof(g_cfg.strip.type));
    strlcpy(g_cfg.strip.order, newOrder, sizeof(g_cfg.strip.order));
    memcpy(g_cfg.segs, newSegs, sizeof(g_cfg.segs));
    g_cfg.numSegs = newNumSegs;
    portEXIT_CRITICAL(&g_ledMux);

    config_save(true);
    req->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    delay(300);
    ESP.restart();
}

// ── Init ─────────────────────────────────────────────────────────────────────
void webui_begin() {
    _srv.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    _srv.on("/api/status", HTTP_GET, handleStatus);

    _srv.on("/api/seg", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        handleSegPost
    );
    _srv.on("/api/global", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        handleGlobalPost
    );
    _srv.on("/api/net", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        handleNetPost
    );
    _srv.on("/api/leds", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        handleLedsPost
    );

    // /api/cfg — read current config as JSON
    _srv.on("/api/cfg", HTTP_GET, [](AsyncWebServerRequest* req) {
        REQUIRE_AUTH(req);

        JsonDocument doc;
        JsonObject net = doc["net"].to<JsonObject>();
        net["dhcp"]   = g_cfg.net.ethDhcp;
        net["ip"]     = g_cfg.net.ethIp;
        net["mask"]   = g_cfg.net.ethMask;
        net["gw"]     = g_cfg.net.ethGw;
        net["ssid"]   = g_cfg.net.wifiSsid;
        net["ap"]     = g_cfg.net.apSsid;
        net["apPassSet"] = (g_cfg.net.apPass[0] != '\0');
        net["apEn"]   = g_cfg.net.apEnabled;
        net["mbport"] = g_cfg.net.modbusPort;
        JsonObject strip = doc["strip"].to<JsonObject>();
        strip["pin"]    = g_cfg.strip.pin;
        strip["count"]  = g_cfg.strip.count;
        strip["briMax"] = g_cfg.strip.briMax;
        strip["type"]   = g_cfg.strip.type;
        JsonArray segs = doc["segs"].to<JsonArray>();
        for (int i = 0; i < g_cfg.numSegs && i < LED_MAX_SEGS; i++) {
            JsonObject s = segs.add<JsonObject>();
            s["s"] = g_cfg.segs[i].start;
            s["e"] = g_cfg.segs[i].end;
        }
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // GET /api/mode  POST /api/mode {"mode":0/1/2, "artnet":{"univ":0,"grp":3}}
    _srv.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* req) {
        REQUIRE_AUTH(req);

        JsonDocument doc;
        doc["mode"] = g_cfg.mode;
        doc["artnet"]["univ"] = g_cfg.artnet.universe;
        doc["artnet"]["grp"]  = g_cfg.artnet.groupSize;
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });
    _srv.on("/api/mode", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            REQUIRE_AUTH(req);

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, data, len);
            if (err) {
                Serial.printf("[WEB] JSON error in /api/mode: %s\n", err.c_str());
                req->send(400, "text/plain", err.c_str());
                return;
            }
            uint8_t newMode = doc["mode"] | g_cfg.mode;
            if (newMode > 2) { req->send(400, "text/plain", "mode must be 0-2"); return; }
            if (!doc["artnet"].isNull()) {
                uint8_t groupSize = doc["artnet"]["grp"] | g_cfg.artnet.groupSize;
                if (groupSize < 1 || groupSize > ARTNET_MAX_GROUPS) {
                    req->send(400, "text/plain", "artnet.grp must be 1-100");
                    return;
                }
                g_cfg.artnet.universe  = doc["artnet"]["univ"] | g_cfg.artnet.universe;
                g_cfg.artnet.groupSize = groupSize;
            }
            if (g_cfg.mode == MODE_ARTNET) artnet_stop();
            g_cfg.mode = newMode;
            config_save(true);
            req->send(200, "application/json", "{\"ok\":true,\"restart\":true}");
            delay(300);
            ESP.restart();
        }
    );

    // OTA firmware upload: POST /update  (multipart, field "firmware")
    _srv.on("/update", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            REQUIRE_AUTH(req);

            bool ok = !Update.hasError();
            req->send(200, "application/json",
                      ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"Update failed\"}");
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data,
           size_t len, bool final) {
            if (!requestHasValidAuth(req)) {
                return;
            }
            if (!index) {
                Serial.printf("[OTA] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Serial.println("[OTA] begin failed");
                }
            }
            if (Update.write(data, len) != len) {
                Serial.println("[OTA] write error");
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Done: %u bytes\n", index + len);
                } else {
                    Serial.println("[OTA] end error");
                }
            }
        }
    );

    // POST /api/relay {"on":true/false}
    _srv.on("/api/relay", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            REQUIRE_AUTH(req);

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, data, len);
            if (err) {
                Serial.printf("[WEB] JSON error in /api/relay: %s\n", err.c_str());
                req->send(400, "text/plain", err.c_str());
                return;
            }
            bool on = doc["on"] | leds_getRelay();
            leds_setRelay(on);
            g_cfg.net.relayOn = on;
            config_save(true);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // GET /api/auth — check if password is set
    _srv.on("/api/auth", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["user"] = g_cfg.auth.username;
        doc["protected"] = config_authConfigured();
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // POST /api/auth — set username/password (requires current auth if already set)
    _srv.on("/api/auth", HTTP_POST,
        [](AsyncWebServerRequest* r){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            bool isProtected = config_authConfigured();
            if (isProtected && !requestHasValidAuth(req)) {
                req->requestAuthentication();
                return;
            }

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, data, len);
            if (err) {
                Serial.printf("[WEB] JSON error in /api/auth: %s\n", err.c_str());
                req->send(400, "text/plain", err.c_str());
                return;
            }

            if (!doc["user"].isNull()) {
                const char* newUser = doc["user"].as<const char*>();
                if (strlen(newUser) < 1 || strlen(newUser) > 32) {
                    req->send(400, "text/plain", "username length must be 1-32");
                    return;
                }
                strlcpy(g_cfg.auth.username, newUser, 33);
            }

            if (!doc["pass"].isNull()) {
                const char* newPass = doc["pass"].as<const char*>();
                if (strlen(newPass) < 8 || strlen(newPass) > 64) {
                    req->send(400, "text/plain", "password length must be 8-64");
                    return;
                }
                config_hashPassword(newPass, g_cfg.auth.passwordHash);
            } else if (!isProtected) {
                req->send(400, "text/plain", "password required");
                return;
            }

            config_save(true);
            req->send(200, "application/json", "{\"ok\":true}");
            Serial.printf("[WEB] Auth updated: user=%s protected=%s\n",
                          g_cfg.auth.username, config_authConfigured() ? "yes" : "no");
        }
    );

    _srv.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    _srv.begin();
    Serial.println("[WEB] HTTP server started on port 80");
}
