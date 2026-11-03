CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wno-unused-parameter -Iinclude -Isrc -Iboard -g -O2

CORE_SRCS = src/arm1176_cpu.c src/arm1176_decode.c src/arm1176_thumb.c src/cp15.c src/vfp.c src/jazelle.c
BOARD_SRCS = board/soc.c
TEST_SRCS = test/main.c test/test_bus.c

all: arm1176_test

arm1176_test: $(CORE_SRCS) $(BOARD_SRCS) $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

run: arm1176_test
	./arm1176_test

clean:
	rm -f arm1176_test

.PHONY: all run clean
