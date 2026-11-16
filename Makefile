KDIR ?= /lib/modules/$(shell uname -r)/build
# Follow the target kernel compiler, including when DKMS builds another kernel.
ifneq ($(shell grep -l "^CONFIG_CC_IS_CLANG=y" $(KDIR)/.config 2>/dev/null),)
export LLVM := 1
endif
DESTDIR ?=
PREFIX ?= /usr/local
MODDIR ?= /lib/modules/$(shell uname -r)/updates/xps7390-webcam

.PHONY: all modules softisp clean clean-modules install

all: modules softisp

modules:
	$(MAKE) -C driver/int346f KDIR=$(KDIR)
	$(MAKE) -C driver/ov01a10 KDIR=$(KDIR)
	$(MAKE) -C driver/ipu4p KDIR=$(KDIR)

softisp:
	$(MAKE) -C softisp

clean-modules:
	$(MAKE) -C driver/int346f KDIR=$(KDIR) clean
	$(MAKE) -C driver/ov01a10 KDIR=$(KDIR) clean
	$(MAKE) -C driver/ipu4p KDIR=$(KDIR) clean

clean: clean-modules
	$(MAKE) -C softisp clean

install: all
	install -d $(DESTDIR)$(MODDIR)
	install -m644 driver/int346f/int346f.ko $(DESTDIR)$(MODDIR)/
	install -m644 driver/ov01a10/ov01a10-i346f.ko $(DESTDIR)$(MODDIR)/
	install -m644 driver/ipu4p/intel-ipu4p.ko $(DESTDIR)$(MODDIR)/
	install -m644 driver/ipu4p/intel-ipu4p-isys.ko $(DESTDIR)$(MODDIR)/
	$(MAKE) -C softisp install DESTDIR=$(DESTDIR) PREFIX=$(PREFIX)
