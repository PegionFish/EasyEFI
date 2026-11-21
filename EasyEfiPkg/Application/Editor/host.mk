# S13 Editor 宿主测试（源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# core 经契约缝链接：MockTui（屏幕+按键）+ MockFs（平面内存 FS）+ 真实 EditorModelLib/Utf8Lib。
# UEFI 侧对应 S06/S07/S09 真实后端（glue 装配；S06/S09 合并前 UEFI 链接受阻，见交接）。
ROOT := $(abspath ../../..)
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks \
          -I$(ROOT)/EasyEfiPkg/Application/Editor
MODDIR := $(ROOT)/EasyEfiPkg/Application/Editor
MODSRC := $(MODDIR)/EditorCore.c \
          $(ROOT)/EasyEfiPkg/Library/EditorModelLib/EditorModel.c \
          $(ROOT)/EasyEfiPkg/Library/Utf8Lib/Utf8.c
MOCKS  := $(ROOT)/tests/host/mocks/MockTui.c $(ROOT)/tests/host/mocks/MockFs.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN    := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/EditorCore.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)
