# S05 VolumeModelLib 宿主测试（源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang/gcc + ASan/UBSan）
# 纯逻辑库，无 ops 依赖，不需要 mocks。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks
MODDIR := $(ROOT)/EasyEfiPkg/Library/VolumeModelLib
MODSRC := $(MODDIR)/VolumeModel.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(TESTS) $(MODDIR)/host.mk
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)