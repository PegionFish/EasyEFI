# S02 EditorModelLib 宿主测试（源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# 依赖：真实 Utf8Lib（S01 已合并进 main；MODSRC 含 ../Utf8Lib/Utf8.c）。
# 历史：S01 合并前曾用本目录 Utf8Shim.c 保守垫片 + REAL_UTF8 开关，现已移除。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host
MODDIR := $(ROOT)/EasyEfiPkg/Library/EditorModelLib
MODSRC := $(MODDIR)/EditorModel.c $(ROOT)/EasyEfiPkg/Library/Utf8Lib/Utf8.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN    := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(TESTS)
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
