# S12 FileManager 宿主测试（模板源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# core 直驱（FmCoreHandleKey），MockTui/MockFs/MockExec 提供双环境后端。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks
# ---- 模块定制 ----
MODDIR := $(ROOT)/EasyEfiPkg/Application/FileManager
MODSRC := $(MODDIR)/FmCore.c
MOCKS  := $(ROOT)/tests/host/mocks/MockTui.c $(ROOT)/tests/host/mocks/MockFs.c $(ROOT)/tests/host/mocks/MockExec.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/FmCore.h $(ROOT)/tests/host/test.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM
