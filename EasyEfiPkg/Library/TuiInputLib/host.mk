# S07 TuiInputLib 宿主测试（模板源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# 两个二进制：run-tests（T07-1..3 + T07-4a 常量）、run-parity（T07-4b 与 MockTui 全量 parity）。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks
# ---- 模块定制 ----
MODDIR := $(ROOT)/EasyEfiPkg/Library/TuiInputLib
MODSRC := $(MODDIR)/TuiInput.c
TESTS  := $(MODDIR)/tests/test_tuiinput.c
PARITY := $(MODDIR)/tests/test_mock_parity.c
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN    := $(MODDIR)/tests/run-tests
BIN2   := $(MODDIR)/tests/run-parity

# parity 二进制在 test_mock_parity.c 内 include MockTui.c + TuiInput.c，不重复列源
$(BIN): $(MODSRC) $(COMPAT) $(TESTS) $(ROOT)/tests/host/test.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT)

$(BIN2): $(MODSRC) $(COMPAT) $(PARITY) $(ROOT)/tests/host/test.h $(ROOT)/tests/host/mocks/MockTui.c
	$(CC) $(CFLAGS) -o $@ $(PARITY) $(COMPAT)

.PHONY: test clean
test: $(BIN) $(BIN2)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN2)
clean:
	rm -f $(BIN) $(BIN2)
