# S11 Launcher core 宿主测试（源自 tests/host/Makefile.template，路径已调整）。
# 运行（在本目录内）: make -f host.mk test
# core 纯逻辑：MockTui/MockExec/MockFs + S04 真实 ExecPath.c；glue（UEFI）不参与宿主编译。
ROOT := $(abspath ../../..)
MODDIR := $(ROOT)/EasyEfiPkg/Application/Launcher
CC ?= cc
CFLAGS := -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks \
          -I$(MODDIR)
# ---- 按模块调整 ----
MODSRC := $(MODDIR)/LauncherCore.c $(ROOT)/EasyEfiPkg/Library/ExecPathLib/ExecPath.c
MOCKS  := $(ROOT)/tests/host/mocks/MockTui.c $(ROOT)/tests/host/mocks/MockExec.c \
          $(ROOT)/tests/host/mocks/MockFs.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
# --------------------
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/LauncherCore.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM
