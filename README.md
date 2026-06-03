# LEDCon v2.0

Industrial LED segment controller firmware for the **Gledopto Elite 2D-EXMU** (ESP32-PoE / LAN8720 Ethernet). Controls WS2812B LED strips via Modbus TCP, Art-Net DMX, or a REST Web API — selectable at runtime.

---

## Hardware

| Component | Details |
|---|---|
| Board | Gledopto Elite 2D-EXMU (GL-C-618WL) |
| MCU | ESP32 240 MHz |
| Ethernet | LAN8720 RMII (PoE capable) |
| LED output | GPIO16 (WS2812B, up to 300 px) |
| Relay | GPIO18 — switches LED strip power supply |
| ETH PHY power | GPIO5 |
| ETH clock | GPIO0 (external 50 MHz from LAN8720) |
| ETH MDC/MDIO | GPIO23 / GPIO33 |

---

## Features

- **Three selectable control protocols** — only one active at a time
  - **Modbus TCP** — industrial standard, FC03/06/16, up to 4 connections
  - **Art-Net** (DMX over UDP) — 4 channels per pixel group: R, G, B, Master-Dimmer
  - **Web API** — REST/JSON, alarm-lamp style segment control
- **Ethernet (DHCP or static)** + **WiFi Access Point** fallback
- **8 configurable LED segments** with start/end positions
- **8 LED effects** per segment (Static, Flash, Pulse, Strobe, Chase)
- **Boot indicator** — orange blink (no network) → blue pulse 10 s (connected) → off
- **Relay control** (GPIO18) via UI or API — persisted in config
- **OTA firmware update** via Web UI
- **Web UI auto-reset** after 5 minutes of inactivity (temporary test mode)
- **LittleFS** filesystem for HTML pages and config.json
- **Wokwi simulation** support (WiFi fallback, GPIO4)

---

## Building & Flashing

### Requirements

- [PlatformIO](https://platformio.org/) (CLI or VSCode extension)
- USB-to-Serial adapter connected to the Gledopto board
- CH340 driver installed (macOS: `brew install --cask wch-ch34x-usb-serial-driver`)

### First-time Flash

```bash
# Build and flash firmware
pio run -e ledcon_hw -t upload

# Flash filesystem (HTML pages)
pio run -e ledcon_hw -t uploadfs

# Open serial monitor
pio device monitor -e ledcon_hw
```

### Board in Bootloader Mode

Hold **BOOT**, press **RESET**, release **BOOT** — then run the upload command.

### Wokwi Simulation

```bash
pio run -e ledcon_wokwi -t upload
```

Uses WiFi (WLAN "Wokwi-GUEST"), LED on GPIO4, no Ethernet.

---

## Network Configuration

### Default Settings

| Parameter | Value |
|---|---|
| Ethernet IP | `192.168.10.10` (static) |
| Subnet | `255.255.255.0` |
| Gateway | `192.168.10.1` |
| WiFi AP | `LEDcon` (open, no password) |
| AP IP | `192.168.10.1` |

Change via Web UI → **Netzwerk** or `POST /api/net`.

### First Access

1. Connect a network cable — board gets `192.168.10.10`
2. Or connect to WiFi **"LEDcon"** → `http://192.168.10.1`
3. Set DHCP / static IP in the Web UI if needed

---

## Web UI

| Page | URL | Description |
|---|---|---|
| Dashboard | `/` | Protocol status, global control, segment cards |
| Netzwerk | `/net.html` | IP config, DHCP, AP settings |
| LED Setup | `/leds.html` | LED count, segment positions |
| Einstellungen | `/settings.html` | Protocol selection, Art-Net config |
| Hilfe | `/help.html` | Full API reference, register tables, OTA update |

---

## Modbus TCP

| Parameter | Value |
|---|---|
| Port | 502 |
| Unit-ID | 1 |
| Function Codes | FC03 (read), FC06 (write single), FC16 (write multiple) |

### Register Map

| Register | Name | Range | R/W |
|---|---|---|---|
| 0 | Global Enable | 0=OFF, 1=ON | R/W |
| 1 | Global Brightness | 0–255 | R/W |
| 2 | Number of Segments | 1–8 | R |
| 10+n×10 | Segment n — Red | 0–255 | R/W |
| 11+n×10 | Segment n — Green | 0–255 | R/W |
| 12+n×10 | Segment n — Blue | 0–255 | R/W |
| 13+n×10 | Segment n — Brightness | 0–255 | R/W |
| 14+n×10 | Segment n — Effect | 0–8 | R/W |
| 15+n×10 | Segment n — Enabled | 0/1 | R/W |

**Segments 0–7** use base addresses 10, 20, 30, 40, 50, 60, 70, 80.

**Effects:** `0`=Off `1`=Static `2`=Flash 4Hz `3`=Flash 0.5Hz `4`=Pulse 2Hz `5`=Pulse 0.33Hz `6`=Strobe 20Hz `7`=Pulse 1Hz `8`=Chase

```
# Example: Set segment 0 to solid red
FC16  Addr=10  Count=6  Data: 255, 0, 0, 200, 1, 1
```

---

## Art-Net

| Parameter | Value |
|---|---|
| UDP Port | 6454 |
| Channels per group | 4 (R, G, B, Master-Dimmer) |
| Universe | Configurable (default: 0) |
| Group size | Configurable LEDs per group (default: 3) |

Dimmer channel: `0` = off, `255` = full brightness.  
Final color = RGB × (Dimmer ÷ 255).

**Channel mapping (groupSize=3, 50 LEDs):**

| Group | DMX Channels | LEDs |
|---|---|---|
| 0 | 1–4 (R,G,B,Dim) | 0–2 |
| 1 | 5–8 | 3–5 |
| n | n×4+1 … n×4+4 | n×grp … |

Configure in Web UI → **Einstellungen**.

---

## REST API

Base URL: `http://192.168.10.10`

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/status` | Current status (IP, mode, relay, segments) |
| GET | `/api/cfg` | Full configuration |
| POST | `/api/seg?n=0` | Set segment n `{"r":255,"g":0,"b":0,"bri":200,"fx":1,"en":true}` |
| POST | `/api/global` | Global control `{"en":true,"bri":200}` |
| POST | `/api/relay` | Relay `{"on":true}` |
| POST | `/api/mode` | Switch protocol `{"mode":0}` (0=WebAPI, 1=ArtNet, 2=Modbus) |
| POST | `/api/net` | Save network config (triggers restart) |
| POST | `/update` | OTA firmware upload (multipart, field `firmware`) |

---

## OTA Update

1. Open `http://192.168.10.10/help.html` → **OTA Update** tab
2. Select `.pio/build/ledcon_hw/firmware.bin`
3. Click **Firmware hochladen**
4. Board restarts automatically after successful upload

---

## Project Structure

```
LEDCon/
├── src/
│   ├── main.cpp          # Setup, loop, protocol dispatch
│   ├── config.h/cpp      # Config struct, LittleFS JSON load/save
│   ├── net.h/cpp         # Ethernet (LAN8720) + WiFi AP
│   ├── leds.h/cpp        # NeoPixelBus driver, boot animation, Art-Net buffer
│   ├── artnet.h/cpp      # Art-Net UDP receiver
│   ├── modbus.h/cpp      # Modbus TCP server (eModbus)
│   ├── webui.h/cpp       # AsyncWebServer, REST API, OTA
├── data/                 # LittleFS web pages
│   ├── index.html        # Dashboard
│   ├── net.html          # Network settings
│   ├── leds.html         # LED setup
│   ├── settings.html     # Protocol selection, Art-Net config
│   └── help.html         # API docs, Modbus register table, OTA
├── platformio.ini        # Build environments (ledcon_hw, ledcon_wokwi)
├── diagram.json          # Wokwi circuit diagram
└── wokwi.toml            # Wokwi simulator config
```

---

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| `makuna/NeoPixelBus` | ^2.7 | WS2812B LED driver (RMT) |
| `me-no-dev/AsyncTCP` | ^1.1 | Async TCP base |
| `mathieucarbou/ESP Async WebServer` | ^3 | HTTP server |
| `bblanchon/ArduinoJson` | ^7 | JSON serialization |
| `miq19/eModbus` | latest | Modbus TCP server |

---

## Build Flags

| Flag | Default | Description |
|---|---|---|
| `LED_DATA_PIN` | 16 (hw) / 4 (sim) | LED data GPIO |
| `LED_MAX_SEGS` | 8 | Maximum segments |
| `LED_MAX_COUNT` | 300 | Maximum LED pixels |
| `LED_DEFAULT_COUNT` | 50 | Default pixel count |
| `MODBUS_PORT` | 502 | Modbus TCP port |
| `LEDCON_VERSION` | 2.0.0 | Firmware version string |
| `WOKWI_SIM` | — | Enable simulation mode (WiFi only) |

---

## Boot Sequence

1. **Orange blink** — waiting for Ethernet link
2. **Blue slow pulse** (1 Hz, 10 s) — network connected, servers starting
3. **All LEDs off** — normal operation, waiting for Modbus/Art-Net/API commands

Relay (GPIO18) is set based on saved config at power-on (default: ON).

---

## License

MIT — see [LICENSE](LICENSE) for details.
