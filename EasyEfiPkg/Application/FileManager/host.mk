# S12 FileManager 宿主测试（模板源自 tests/host/Makefile.template）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# 双后端各跑一遍 T12-1..11：
#   run-tests       MockFs（内存平面表）
#   run-tests-posix S09 FsPosixBackend（真实临时目录，任务书"POSIX 临时树"）
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
BIN    := $(MODDIR)/tests/run-tests
BIN2   := $(MODDIR)/tests/run-tests-posix
# POSIX 后端（S09 宿主源码）+ 其依赖 FsCommon/VolumeModel（S05 真实实现）
EVDIR  := $(ROOT)/EasyEfiPkg/Library/ExecVolumeLib
POSIXSRC := $(EVDIR)/FsCommon.c $(EVDIR)/FsPosixBackend.c \
            $(ROOT)/EasyEfiPkg/Library/VolumeModelLib/VolumeModel.c
POSIXINC := -I$(EVDIR) -DUSE_POSIX_FS

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/FmCore.h $(ROOT)/tests/host/test.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

$(BIN2): $(MODSRC) $(COMPAT) $(TESTS) $(POSIXSRC) $(MODDIR)/FmCore.h $(ROOT)/tests/host/test.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) $(POSIXINC) -o $@ $(TESTS) $(MODSRC) $(COMPAT) \
	  $(ROOT)/tests/host/mocks/MockTui.c $(ROOT)/tests/host/mocks/MockExec.c $(POSIXSRC)

.PHONY: test clean
test: $(BIN) $(BIN2)
	@echo "== MockFs backend =="
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
	@echo "== POSIX temp-tree backend =="
	ASAN_OPTIONS=detect_leaks=0 $(BIN2)
clean:
	rm -f $(BIN) $(BIN2)
	rm -rf $(BIN).dSYM $(BIN2).dSYM
