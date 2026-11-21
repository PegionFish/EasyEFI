# TuiWidgetLib 宿主测试（源自 tests/host/Makefile.template，S08 按模块调整）
# 运行: make -f host.mk test   （在模块目录内；原生 clang + ASan/UBSan）
# mock 屏幕/按键 = tests/host/mocks/MockTui.c（leader 独占，只读链接）
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks
# ---- 按模块调整 ----
MODDIR := $(ROOT)/EasyEfiPkg/Library/TuiWidgetLib
MODSRC := $(MODDIR)/TuiWidget.c
MOCKS  := $(ROOT)/tests/host/mocks/MockTui.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
# --------------------
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS)
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM
