#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "net.h"
#include "leds.h"
#include "modbus.h"
#include "artnet.h"
#include "webui.h"

#ifndef LEDCON_VERSION
#define LEDCON_VERSION "2.0.0"
#endif

static bool _serverStarted = false;

void setup() {
    Serial.begin(115200);
    config_load();
    leds_begin();  // LEDs first, immediately — same as WLED
    Serial.printf("\n\n=== LEDCon v%s ===\n", LEDCON_VERSION);
    net_begin();
}

void loop() {
    esp_task_wdt_reset();
    net_loop();
    leds_loop();

    // Start Modbus + WebUI once network is up
    if (!_serverStarted && net_connected()) {
        Serial.printf("[MAIN] Network up — starting servers (IP: %s)\n",
                      net_localIP().toString().c_str());
        Serial.flush();
        switch (g_cfg.mode) {
            case MODE_MODBUS: modbus_begin(); break;
            case MODE_ARTNET: artnet_begin(); break;
            case MODE_WEBAPI: break;  // only WebUI
        }
        webui_begin();
        leds_onNetUp();
        _serverStarted = true;
    }

    // Periodic heartbeat so serial monitor shows something even after boot
    static uint32_t _lastHb = 0;
    uint32_t now = millis();
    if (now - _lastHb >= 5000) {
        _lastHb = now;
        Serial.printf("[HB] uptime=%lus  heap=%u  servers=%s  ip=%s\n",
                      now / 1000,
                      ESP.getFreeHeap(),
                      _serverStarted ? "UP" : "WAIT",
                      net_localIP().toString().c_str());
        Serial.flush();
    }
}
