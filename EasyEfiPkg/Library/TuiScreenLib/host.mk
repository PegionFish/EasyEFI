# S06 TuiScreenLib 宿主测试（Makefile.template 适配）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# 核心 TuiScreen.c 双环境共享；UEFI 后端 TuiScreenUefi.c 不参与宿主构建，
# 由 tests/MockConOut.c 提供链接期 TuiScreenBackend()。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host \
          -I$(ROOT)/EasyEfiPkg/Library/TuiScreenLib \
          -I$(ROOT)/EasyEfiPkg/Library/TuiScreenLib/tests
MODDIR := $(ROOT)/EasyEfiPkg/Library/TuiScreenLib
MODSRC := $(MODDIR)/TuiScreen.c
MOCKS  := $(MODDIR)/tests/MockConOut.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/TuiScreenInternal.h $(MODDIR)/tests/MockConOut.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(MOCKS) $(COMPAT)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
