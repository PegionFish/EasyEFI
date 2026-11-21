# S09 ExecVolumeLib 宿主测试（POSIX 后端 + 卷层纯逻辑核心）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan）
# VolumeModel 契约实现自动选择：S05 真实源码（非 Stub）在场则用之，
# 否则用 S09 INTERIM（VolumeModelInterim.c，契约语义一致）。
ROOT := $(abspath ../../..)
MODDIR := $(ROOT)/EasyEfiPkg/Library/ExecVolumeLib
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(MODDIR)

MODSRC := $(MODDIR)/FsCommon.c $(MODDIR)/FsPosixBackend.c
COMPAT := $(MODDIR)/tests/TestMem.c

VM_ALL  := $(wildcard $(ROOT)/EasyEfiPkg/Library/VolumeModelLib/*.c)
VM_REAL := $(filter-out %Stub.c,$(VM_ALL))
ifeq ($(VM_REAL),)
VMSRC := $(MODDIR)/VolumeModelInterim.c
else
VMSRC := $(VM_REAL)
endif

TESTS := $(wildcard $(MODDIR)/tests/test_*.c)
BINS  := $(patsubst $(MODDIR)/tests/test_%.c,$(MODDIR)/tests/run-%,$(TESTS))

.PHONY: test clean
test: $(BINS)
	@for b in $(BINS); do \
	  echo "== $$b =="; \
	  ASAN_OPTIONS=detect_leaks=0 $$b || exit 1; \
	done

$(MODDIR)/tests/run-%: $(MODDIR)/tests/test_%.c $(MODSRC) $(COMPAT) $(VMSRC) \
                       $(MODDIR)/ExecVolumeInternal.h $(MODDIR)/tests/TestUtil.h $(MODDIR)/tests/DpSynth.h
	$(CC) $(CFLAGS) -o $@ $< $(MODSRC) $(COMPAT) $(VMSRC)

clean:
	rm -f $(BINS)
