# S02 EditorModelLib 宿主测试（源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test            （模块目录内；原生 clang + ASan/UBSan）
#
# 依赖两阶段（S01 Utf8Lib 并行开发）：
#   REAL_UTF8=0（当前默认）: MODSRC 含本目录 Utf8Shim.c（保守解码：非 ASCII 全 RAW）。
#     字节往返/行尾/BOM/撤销/限额/渲染占位等用例全绿；中文 CP 级子用例编译期跳过。
#   REAL_UTF8=1（leader 合并 S01 后）: MODSRC 换 ../Utf8Lib/Utf8.c + -DS02_REAL_UTF8，
#     全部用例（含中文 needle/单格占位/码点列）启用。切换后默认值应改为 1 并删除垫片。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host
MODDIR := $(ROOT)/EasyEfiPkg/Library/EditorModelLib
REAL_UTF8 ?= 0
ifeq ($(REAL_UTF8),1)
MODSRC := $(MODDIR)/EditorModel.c $(ROOT)/EasyEfiPkg/Library/Utf8Lib/Utf8.c
CFLAGS += -DS02_REAL_UTF8
else
MODSRC := $(MODDIR)/EditorModel.c $(MODDIR)/Utf8Shim.c
endif
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN    := $(MODDIR)/tests/run-tests
# 模式戳：REAL_UTF8 切换时强制重链（避免旧二进制静默复用）
STAMP  := $(MODDIR)/tests/.mode-$(REAL_UTF8)

$(STAMP):
	mkdir -p $(MODDIR)/tests
	rm -f $(MODDIR)/tests/.mode-*
	touch $@

$(BIN): $(STAMP) $(MODSRC) $(COMPAT) $(TESTS)
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN) $(MODDIR)/tests/.mode-*
