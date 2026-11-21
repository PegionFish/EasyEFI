# S01 Utf8Lib 宿主测试（源自 tests/host/Makefile.template，路径已调整）。
# 运行（在本目录内）:
#   make -f host.mk test        标准后端 EfiMemHost.c + ASan/UBSan
#   make -f host.mk test-leaks  T01-10 配对计数后端 tests/mem_tracker.c（存活分配+canary 断言）
# Utf8Lib 纯字节逻辑，无 CHAR16/无 mock 依赖（MOCKS 留空）。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks
# ---- 按模块调整 ----
MODDIR := $(ROOT)/EasyEfiPkg/Library/Utf8Lib
MODSRC := $(MODDIR)/Utf8.c
MOCKS  :=
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
# --------------------
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
TRACKER := $(MODDIR)/tests/mem_tracker.c
BIN := $(MODDIR)/tests/run-tests
BIN_TRACKED := $(MODDIR)/tests/run-tests-tracked

$(BIN): $(MODSRC) $(COMPAT) $(TESTS)
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

$(BIN_TRACKED): $(MODSRC) $(TRACKER) $(TESTS)
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -DMEM_TRACKED -o $@ $(TESTS) $(MODSRC) $(TRACKER)

.PHONY: test test-leaks clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
test-leaks: $(BIN_TRACKED)
	ASAN_OPTIONS=detect_leaks=0 $(BIN_TRACKED)
clean:
	rm -f $(BIN) $(BIN_TRACKED)
