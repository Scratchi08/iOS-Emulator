#ifndef TEST_BUS_H
#define TEST_BUS_H

#include <stdint.h>
#include "arm1176.h"

typedef struct {
    uint8_t *mem;
    uint32_t size;
} test_bus_t;

void test_bus_init(test_bus_t *tb, uint32_t size);
void test_bus_free(test_bus_t *tb);
arm_bus_t test_bus_get_interface(test_bus_t *tb);

#endif
