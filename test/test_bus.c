#include <stdlib.h>
#include <string.h>
#include "test_bus.h"

static uint8_t tb_read8(void *ctx, uint32_t addr) {
    test_bus_t *tb = ctx;
    return (addr < tb->size) ? tb->mem[addr] : 0;
}
static uint16_t tb_read16(void *ctx, uint32_t addr) {
    test_bus_t *tb = ctx;
    if (addr + 1 >= tb->size) return 0;
    return (uint16_t)(tb->mem[addr] | (tb->mem[addr + 1] << 8));
}
static uint32_t tb_read32(void *ctx, uint32_t addr) {
    test_bus_t *tb = ctx;
    if (addr + 3 >= tb->size) return 0;
    return (uint32_t)tb->mem[addr]
         | ((uint32_t)tb->mem[addr + 1] << 8)
         | ((uint32_t)tb->mem[addr + 2] << 16)
         | ((uint32_t)tb->mem[addr + 3] << 24);
}
static void tb_write8(void *ctx, uint32_t addr, uint8_t val) {
    test_bus_t *tb = ctx;
    if (addr < tb->size) tb->mem[addr] = val;
}
static void tb_write16(void *ctx, uint32_t addr, uint16_t val) {
    test_bus_t *tb = ctx;
    if (addr + 1 < tb->size) { tb->mem[addr] = val & 0xFF; tb->mem[addr+1] = val >> 8; }
}
static void tb_write32(void *ctx, uint32_t addr, uint32_t val) {
    test_bus_t *tb = ctx;
    if (addr + 3 < tb->size) {
        tb->mem[addr]   = val & 0xFF;
        tb->mem[addr+1] = (val >> 8) & 0xFF;
        tb->mem[addr+2] = (val >> 16) & 0xFF;
        tb->mem[addr+3] = (val >> 24) & 0xFF;
    }
}

void test_bus_init(test_bus_t *tb, uint32_t size) {
    tb->mem = calloc(1, size);
    tb->size = size;
}
void test_bus_free(test_bus_t *tb) {
    free(tb->mem);
    tb->mem = NULL;
}
arm_bus_t test_bus_get_interface(test_bus_t *tb) {
    arm_bus_t b;
    b.read8 = tb_read8;   b.read16 = tb_read16;   b.read32 = tb_read32;
    b.write8 = tb_write8; b.write16 = tb_write16; b.write32 = tb_write32;
    b.ctx = tb;
    return b;
}
