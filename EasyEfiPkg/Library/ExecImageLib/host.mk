# S10 ExecImageLib 宿主测试（S00 模板派生）。运行: make -f host.mk test
ROOT := $(abspath ../../..)
MODDIR := $(ROOT)/EasyEfiPkg/Library/ExecImageLib
CC ?= cc
CFLAGS := -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
          -I$(ROOT)/EasyEfiPkg/Include -I$(ROOT)/tests/host -I$(ROOT)/tests/host/mocks \
          -I$(MODDIR)
MODSRC := $(MODDIR)/ExecImage.c
MOCKS  := $(MODDIR)/tests/HostSeam.c $(MODDIR)/tests/HostVol.c \
          $(MODDIR)/fixture/VolModelFixture.c $(ROOT)/tests/host/mocks/MockFs.c
TESTS  := $(wildcard $(MODDIR)/tests/test_*.c)
COMPAT := $(ROOT)/EasyEfiPkg/Library/EfiCompatLib/host/EfiMemHost.c
BIN := $(MODDIR)/tests/run-tests

$(BIN): $(MODSRC) $(COMPAT) $(MOCKS) $(TESTS) $(MODDIR)/ExecImageSeam.h
	mkdir -p $(MODDIR)/tests
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODSRC) $(COMPAT) $(MOCKS)

.PHONY: test clean
test: $(BIN)
	ASAN_OPTIONS=detect_leaks=0 $(BIN)
clean:
	rm -f $(BIN)