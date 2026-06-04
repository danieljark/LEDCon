#include "artnet.h"
#include "leds.h"
#include "config.h"
#include <AsyncUDP.h>

#define ARTNET_PORT 6454

static AsyncUDP _udp;

// Art-Net OpDmx: write DMX data to LED pixel groups
// Each group = groupSize LEDs, 4 channels: R, G, B, Dimmer
static void onPacket(AsyncUDPPacket& pkt) {
    uint8_t* d   = pkt.data();
    size_t   len = pkt.length();

    if (len < 18) return;
    if (memcmp(d, "Art-Net\0", 8) != 0) return;

    uint16_t opcode = d[8] | ((uint16_t)d[9] << 8);
    if (opcode != 0x5000) return;  // OpDmx only

    uint16_t universe = d[14] | ((uint16_t)d[15] << 8);
    if (universe != g_cfg.artnet.universe) return;

    uint16_t dmxLen = ((uint16_t)d[16] << 8) | d[17];
    if (len < (size_t)(18 + dmxLen)) return;

    uint8_t*  dmx       = d + 18;
    uint16_t  grpSize   = max((uint8_t)1, g_cfg.artnet.groupSize);
    uint16_t  ledCount  = g_cfg.strip.count;
    uint16_t  numGroups = (ledCount + grpSize - 1) / grpSize;

    uint16_t lastLed = 0;
    for (uint16_t g = 0; g < numGroups; g++) {
        uint16_t ch = g * 4;
        if (ch + 3 >= dmxLen) break;

        float   dim = dmx[ch + 3] / 255.0f;
        uint8_t r   = (uint8_t)(dmx[ch]     * dim);
        uint8_t gn  = (uint8_t)(dmx[ch + 1] * dim);
        uint8_t b   = (uint8_t)(dmx[ch + 2] * dim);

        uint16_t start = g * grpSize;
        uint16_t end   = min((uint16_t)(start + grpSize - 1), (uint16_t)(ledCount - 1));

        leds_writeArtNetGroup(start, end, r, gn, b);
        lastLed = end + 1;
    }

    if (lastLed < ledCount) {
        leds_clearArtNetBuffer(lastLed, ledCount - 1);
    }
    leds_flushArtNet();
}

void artnet_begin() {
    if (_udp.listen(ARTNET_PORT)) {
        _udp.onPacket(onPacket);
        Serial.printf("[ArtNet] Listening on UDP %d, universe=%d, groupSize=%d\n",
                      ARTNET_PORT, g_cfg.artnet.universe, g_cfg.artnet.groupSize);
    } else {
        Serial.println("[ArtNet] Failed to bind UDP port");
    }
}

void artnet_stop() {
    _udp.close();
}
