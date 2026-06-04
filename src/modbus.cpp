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
// Reg 10+n*10 : Seg n R
// Reg 11+n*10 : Seg n G
// Reg 12+n*10 : Seg n B
// Reg 13+n*10 : Seg n Brightness
// Reg 14+n*10 : Seg n Effect (0-8)
// Reg 15+n*10 : Seg n Enabled (0/1)

static ModbusServerImpl _server;

static uint16_t readReg(uint16_t addr) {
    if (addr == 0) return g_pGlobalEn  ? 1 : 0;
    if (addr == 1) return g_pGlobalBri;
    if (addr == 2) return g_numSegs;
    if (addr >= MODBUS_REGISTER_BASE_SEGMENTS && addr < MODBUS_MAX_REGISTER_ADDR) {
        uint8_t seg = (addr - MODBUS_REGISTER_BASE_SEGMENTS) / MODBUS_REGISTER_STRIDE;
        uint8_t fld = (addr - MODBUS_REGISTER_BASE_SEGMENTS) % MODBUS_REGISTER_STRIDE;
        if (seg >= LED_MAX_SEGS) return 0;
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

static void writeReg(uint16_t addr, uint16_t val) {
    if (addr == 0) { leds_writeGlobal(val != 0, g_pGlobalBri); return; }
    if (addr == 1) { leds_writeGlobal(g_pGlobalEn, (uint8_t)min(val,(uint16_t)255)); return; }
    if (addr >= MODBUS_REGISTER_BASE_SEGMENTS && addr < MODBUS_MAX_REGISTER_ADDR) {
        uint8_t seg = (addr - MODBUS_REGISTER_BASE_SEGMENTS) / MODBUS_REGISTER_STRIDE;
        uint8_t fld = (addr - MODBUS_REGISTER_BASE_SEGMENTS) % MODBUS_REGISTER_STRIDE;
        if (seg >= LED_MAX_SEGS) return;
        portENTER_CRITICAL(&g_ledMux);
        SegState s = g_pSegs[seg];
        portEXIT_CRITICAL(&g_ledMux);
        switch (fld) {
            case 0: s.r   = (uint8_t)min(val,(uint16_t)255); break;
            case 1: s.g   = (uint8_t)min(val,(uint16_t)255); break;
            case 2: s.b   = (uint8_t)min(val,(uint16_t)255); break;
            case 3: s.bri = (uint8_t)min(val,(uint16_t)255); break;
            case 4: s.fx  = (uint8_t)min(val,(uint16_t)8);   break;
            case 5: s.en  = (val != 0);                       break;
            default: return;
        }
        leds_writeSeg(seg, s);
    }
}

// ── FC03 — Read Holding Registers ────────────────────────────────────────────
static ModbusMessage FC03(ModbusMessage req) {
    uint16_t addr, count;
    req.get(2, addr);
    req.get(4, count);

    if (count == 0 || count > MODBUS_MAX_READ_COUNT || addr + count > MODBUS_MAX_REGISTER_ADDR) {
        ModbusMessage err;
        err.setServerID(req.getServerID());
        err.setFunctionCode(req.getFunctionCode() | 0x80);
        err.add((uint8_t)0x02);  // ILLEGAL_DATA_ADDRESS
        return err;
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
    uint16_t addr, val;
    req.get(2, addr);
    req.get(4, val);
    writeReg(addr, val);
    return req;  // echo back
}

// ── FC16 — Write Multiple Registers ─────────────────────────────────────────
static ModbusMessage FC16(ModbusMessage req) {
    uint16_t addr, count;
    uint8_t  bytes;
    req.get(2, addr);
    req.get(4, count);
    req.get(6, bytes);

    if (count == 0 || count > MODBUS_MAX_WRITE_COUNT || addr + count > MODBUS_MAX_REGISTER_ADDR) {
        ModbusMessage err;
        err.setServerID(req.getServerID());
        err.setFunctionCode(req.getFunctionCode() | 0x80);
        err.add((uint8_t)0x02);
        return err;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint16_t val; req.get(7 + i * 2, val);
        writeReg(addr + i, val);
    }

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
