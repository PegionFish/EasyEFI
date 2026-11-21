# S03 ConfigCoreLib 宿主测试（模板 tests/host/Makefile.template 派生）。
# 运行: make -f host.mk test   （模块目录内；原生 clang + ASan/UBSan + 内存配对计数）
# jansson = 共享 edk2 子模块源码（与 UEFI JsonLib 同版本 2.13.1），原生编译进测试二进制。
# ★ S18 合并 S04 后：PATHSTUB 改为 ExecPathLib 真实源码（见交接注记）。
ROOT := $(abspath ../../..)
CC ?= cc
MODDIR := $(ROOT)/EasyEfiPkg/Library/ConfigCoreLib
JANSSON_SRC ?= $(HOME)/.cache/easyefi/edk2/RedfishPkg/Library/JsonLib/jansson/src

CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host \
          -I$(MODDIR) -I$(MODDIR)/host -I$(JANSSON_SRC) \
          -DHAVE_CONFIG_H=1 -DGOLDEN_PATH=\"$(ROOT)/config/launcher.example.json\" \
          -DGOLDEN_DUMP_PATH=\"$(MODDIR)/tests/golden-dom.txt\"

MODSRC := $(MODDIR)/ConfigCore.c $(MODDIR)/JsonDom.c $(MODDIR)/JsonDomHost.c
JANSSON := $(addprefix $(JANSSON_SRC)/,dump.c error.c hashtable.c hashtable_seed.c \
           load.c memory.c pack_unpack.c strbuffer.c strconv.c utf.c value.c version.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
PATHSTUB := $(MODDIR)/PathParseTemp.c   # ★ 临时：S04 合并后替换
TESTS := $(wildcard $(MODDIR)/tests/test_*.c)

BIN := $(MODDIR)/tests/run-tests
BIN_LEAK := $(MODDIR)/tests/run-tests-leak

$(BIN): $(MODSRC) $(JANSSON) $(COMPAT) $(PATHSTUB) $(TESTS) $(MODDIR)/host.mk
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(JANSSON) $(COMPAT) $(PATHSTUB)

# 内存配对变体：计数分配器替换 EfiMemHost（T03-17 泄漏平衡断言）
$(BIN_LEAK): $(MODSRC) $(JANSSON) $(MODDIR)/tests/MemCount.c $(PATHSTUB) $(TESTS) $(MODDIR)/host.mk
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -DCFG_COUNT_MEM -o $@ $(TESTS) $(MODSRC) $(JANSSON) $(MODDIR)/tests/MemCount.c $(PATHSTUB)

.PHONY: test clean
test: $(BIN) $(BIN_LEAK)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN_LEAK)
clean:
	rm -f $(BIN) $(BIN_LEAK)