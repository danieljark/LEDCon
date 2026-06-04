#include "modbus.h"
#include "leds.h"
#include "config.h"
// ModbusServerTCPasync uses AsyncTCP — works on both WiFi and ETH, no Ethernet lib needed
#include <ModbusServerTCPasync.h>
using ModbusServerImpl = ModbusServerTCPasync;

// ── Register Map ─────────────────────────────────────────────────────────────
// Reg  0   : GlobalEnable   (0/1)
// Reg  1   : GlobalBrightness (0-255)
// Reg  2   : NumSegments     (R)
// Reg  3   : Modbus heartbeat / watchdog kick (W)
// Reg 10+n*10 : Seg n R
// Reg 11+n*10 : Seg n G
// Reg 12+n*10 : Seg n B
// Reg 13+n*10 : Seg n Brightness
// Reg 14+n*10 : Seg n Effect (0-8)
// Reg 15+n*10 : Seg n Enabled (0/1)

static ModbusServerImpl _server;

static ModbusMessage exceptionResponse(ModbusMessage& req, uint8_t code) {
    ModbusMessage err;
    err.setServerID(req.getServerID());
    err.setFunctionCode(req.getFunctionCode() | 0x80);
    err.add(code);
    return err;
}

static bool isSegmentRegister(uint16_t addr, uint8_t* segOut = nullptr, uint8_t* fldOut = nullptr) {
    if (addr < MODBUS_REGISTER_BASE_SEGMENTS || addr >= MODBUS_MAX_REGISTER_ADDR) return false;
    uint8_t seg = (addr - MODBUS_REGISTER_BASE_SEGMENTS) / MODBUS_REGISTER_STRIDE;
    uint8_t fld = (addr - MODBUS_REGISTER_BASE_SEGMENTS) % MODBUS_REGISTER_STRIDE;
    if (seg >= LED_MAX_SEGS || fld > 5) return false;
    if (segOut) *segOut = seg;
    if (fldOut) *fldOut = fld;
    return true;
}

static bool isWritableRegister(uint16_t addr) {
    return addr == 0 || addr == 1 || addr == 3 || isSegmentRegister(addr);
}

static bool isWritableRange(uint16_t addr, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        if (!isWritableRegister(addr + i)) return false;
    }
    return true;
}

static uint16_t readReg(uint16_t addr) {
    if (addr == 0) {
        portENTER_CRITICAL(&g_ledMux);
        uint16_t v = g_pGlobalEn ? 1 : 0;
        portEXIT_CRITICAL(&g_ledMux);
        return v;
    }
    if (addr == 1) {
        portENTER_CRITICAL(&g_ledMux);
        uint16_t v = g_pGlobalBri;
        portEXIT_CRITICAL(&g_ledMux);
        return v;
    }
    if (addr == 2) return g_numSegs;
    if (addr == 3) return 0;
    uint8_t seg, fld;
    if (isSegmentRegister(addr, &seg, &fld)) {
        portENTER_CRITICAL(&g_ledMux);
        SegState s = g_pSegs[seg];
        portEXIT_CRITICAL(&g_ledMux);
        switch (fld) {
            case 0: return s.r;
            case 1: return s.g;
            case 2: return s.b;
            case 3: return s.bri;
            case 4: return s.fx;
            case 5: return s.en ? 1 : 0;
        }
    }
    return 0;
}

static bool writeReg(uint16_t addr, uint16_t val) {
    if (addr == 0) {
        portENTER_CRITICAL(&g_ledMux);
        uint8_t bri = g_pGlobalBri;
        portEXIT_CRITICAL(&g_ledMux);
        leds_writeGlobal(val != 0, bri);
        return true;
    }
    if (addr == 1) {
        portENTER_CRITICAL(&g_ledMux);
        bool en = g_pGlobalEn;
        portEXIT_CRITICAL(&g_ledMux);
        leds_writeGlobal(en, (uint8_t)min(val,(uint16_t)255));
        return true;
    }
    if (addr == 3) { leds_touchModbus(); return true; }
    uint8_t seg, fld;
    if (isSegmentRegister(addr, &seg, &fld)) {
        SegState s{};
        uint8_t mask = 0;
        switch (fld) {
            case 0: s.r   = (uint8_t)min(val,(uint16_t)255); mask = SEG_PATCH_R;   break;
            case 1: s.g   = (uint8_t)min(val,(uint16_t)255); mask = SEG_PATCH_G;   break;
            case 2: s.b   = (uint8_t)min(val,(uint16_t)255); mask = SEG_PATCH_B;   break;
            case 3: s.bri = (uint8_t)min(val,(uint16_t)255); mask = SEG_PATCH_BRI; break;
            case 4: s.fx  = (uint8_t)min(val,(uint16_t)8);   mask = SEG_PATCH_FX;  break;
            case 5: s.en  = (val != 0);                      mask = SEG_PATCH_EN;  break;
            default: return false;
        }
        return leds_patchSeg(seg, mask, s);
    }
    return false;
}

static bool applyRegisterToSnapshot(uint16_t addr, uint16_t val,
                                    bool& globalEn, uint8_t& globalBri,
                                    SegState segs[LED_MAX_SEGS]) {
    if (addr == 0) { globalEn = (val != 0); return true; }
    if (addr == 1) { globalBri = (uint8_t)min(val, (uint16_t)255); return true; }
    if (addr == 3) { return true; }

    uint8_t seg, fld;
    if (!isSegmentRegister(addr, &seg, &fld)) return false;
    switch (fld) {
        case 0: segs[seg].r   = (uint8_t)min(val, (uint16_t)255); break;
        case 1: segs[seg].g   = (uint8_t)min(val, (uint16_t)255); break;
        case 2: segs[seg].b   = (uint8_t)min(val, (uint16_t)255); break;
        case 3: segs[seg].bri = (uint8_t)min(val, (uint16_t)255); break;
        case 4: segs[seg].fx  = (uint8_t)min(val, (uint16_t)8);   break;
        case 5: segs[seg].en  = (val != 0);                       break;
        default: return false;
    }
    return true;
}

// ── FC03 — Read Holding Registers ────────────────────────────────────────────
static ModbusMessage FC03(ModbusMessage req) {
    if (req.size() < 6) return exceptionResponse(req, 0x03);  // ILLEGAL_DATA_VALUE

    uint16_t addr, count;
    req.get(2, addr);
    req.get(4, count);

    if (count == 0 || count > MODBUS_MAX_READ_COUNT ||
        addr >= MODBUS_MAX_REGISTER_ADDR || count > MODBUS_MAX_REGISTER_ADDR - addr) {
        return exceptionResponse(req, 0x02);  // ILLEGAL_DATA_ADDRESS
    }

    ModbusMessage resp;
    resp.setServerID(req.getServerID());
    resp.setFunctionCode(READ_HOLD_REGISTER);
    resp.add((uint8_t)(count * 2));
    for (uint16_t i = 0; i < count; i++) resp.add(readReg(addr + i));
    return resp;
}

// ── FC06 — Write Single Register ─────────────────────────────────────────────
static ModbusMessage FC06(ModbusMessage req) {
    if (req.size() < 6) return exceptionResponse(req, 0x03);

    uint16_t addr, val;
    req.get(2, addr);
    req.get(4, val);
    if (!writeReg(addr, val)) return exceptionResponse(req, 0x02);
    leds_touchModbus();
    return req;  // echo back
}

// ── FC16 — Write Multiple Registers ─────────────────────────────────────────
static ModbusMessage FC16(ModbusMessage req) {
    if (req.size() < 7) return exceptionResponse(req, 0x03);

    uint16_t addr, count;
    uint8_t  bytes;
    req.get(2, addr);
    req.get(4, count);
    req.get(6, bytes);

    if (count == 0 || count > MODBUS_MAX_WRITE_COUNT ||
        addr >= MODBUS_MAX_REGISTER_ADDR || count > MODBUS_MAX_REGISTER_ADDR - addr) {
        return exceptionResponse(req, 0x02);
    }
    if (!isWritableRange(addr, count)) {
        return exceptionResponse(req, 0x02);
    }
    if (bytes != count * 2 || req.size() < 7 + bytes) {
        return exceptionResponse(req, 0x03);  // ILLEGAL_DATA_VALUE
    }

    bool globalEn;
    uint8_t globalBri;
    SegState segs[LED_MAX_SEGS];
    portENTER_CRITICAL(&g_ledMux);
    globalEn = g_pGlobalEn;
    globalBri = g_pGlobalBri;
    memcpy(segs, g_pSegs, sizeof(segs));
    portEXIT_CRITICAL(&g_ledMux);

    for (uint16_t i = 0; i < count; i++) {
        uint16_t val;
        req.get(7 + i * 2, val);
        if (!applyRegisterToSnapshot(addr + i, val, globalEn, globalBri, segs)) {
            return exceptionResponse(req, 0x02);
        }
    }

    portENTER_CRITICAL(&g_ledMux);
    g_pGlobalEn = globalEn;
    g_pGlobalBri = globalBri;
    memcpy(g_pSegs, segs, sizeof(g_pSegs));
    g_pending = true;
    portEXIT_CRITICAL(&g_ledMux);
    leds_touchModbus();

    ModbusMessage resp;
    resp.setServerID(req.getServerID());
    resp.setFunctionCode(WRITE_MULT_REGISTERS);
    resp.add(addr);
    resp.add(count);
    return resp;
}

// ── Init ─────────────────────────────────────────────────────────────────────
void modbus_begin() {
    _server.registerWorker(1, READ_HOLD_REGISTER,  &FC03);
    _server.registerWorker(1, WRITE_HOLD_REGISTER, &FC06);
    _server.registerWorker(1, WRITE_MULT_REGISTERS, &FC16);
    _server.start(g_cfg.net.modbusPort, 4, 10000);
    Serial.printf("[MB] Modbus TCP listening on port %d\n", g_cfg.net.modbusPort);
}
