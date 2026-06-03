#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ETH.h>

static DNSServer _dns;
static bool _ethUp = false;
static bool _apUp  = false;

static IPAddress parseIP(const char* s) {
    IPAddress ip; ip.fromString(s); return ip;
}

static void startAP() {
    WiFi.softAPConfig(
        IPAddress(192,168,10,1),
        IPAddress(192,168,10,1),
        IPAddress(255,255,255,0)
    );
    WiFi.softAP(g_cfg.net.apSsid, "");
    _dns.start(53, "*", IPAddress(192,168,10,1));
    _apUp = true;
    Serial.printf("[NET] AP started: SSID=%s  IP=192.168.10.1\n", g_cfg.net.apSsid);
}

static void onEthEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("[NET] ETH start");
            ETH.setHostname("ledcon");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            _ethUp = true;
            Serial.printf("[NET] ETH up: %s\n", ETH.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            _ethUp = true;
            Serial.printf("[NET] ETH got IP: %s\n", ETH.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            _ethUp = false;
            Serial.println("[NET] ETH disconnected");
            break;
        default: break;
    }
}

static void startWifiSta() {
    if (strlen(g_cfg.net.wifiSsid) > 0) {
        WiFi.begin(g_cfg.net.wifiSsid, g_cfg.net.wifiPass);
        Serial.printf("[NET] WiFi STA: connecting to %s\n", g_cfg.net.wifiSsid);
    }
}

void net_begin() {
    WiFi.onEvent(onEthEvent);
    WiFi.mode(WIFI_AP_STA);

    // Gledopto Elite 2D-EXMU: LAN8720, addr=1, pwr=GPIO5, MDC=23, MDIO=33, CLK=GPIO0_IN
    // Hardcoded — board pins_arduino.h overrides -D build flags for ETH_PHY_* macros
    bool ethOk = ETH.begin(1, 5, 23, 33, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    Serial.printf("[NET] ETH.begin: addr=1 pwr=5 mdc=23 mdio=33 clk=GPIO0_IN → %s\n",
                  ethOk ? "OK" : "FAILED");

    // Static IP must be set AFTER ETH.begin() on ESP32 Arduino 2.x
    if (ethOk && !g_cfg.net.ethDhcp) {
        ETH.config(
            parseIP(g_cfg.net.ethIp),
            parseIP(g_cfg.net.ethGw),
            parseIP(g_cfg.net.ethMask)
        );
    }

    if (g_cfg.net.apEnabled) startAP();
    startWifiSta();
}

void net_loop() {
    if (_apUp) _dns.processNextRequest();
}

IPAddress net_localIP() {
    if (_ethUp) return ETH.localIP();
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP();
    return WiFi.softAPIP();
}

bool net_connected() {
    return _ethUp || (g_cfg.net.apEnabled && _apUp) || (WiFi.status() == WL_CONNECTED);
}
