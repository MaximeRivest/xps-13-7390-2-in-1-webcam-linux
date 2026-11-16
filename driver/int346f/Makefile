KDIR ?= /lib/modules/$(shell uname -r)/build
obj-m += int346f.o
int346f-y := discrete.o common.o clk_and_regulator.o discrete_quirks.o
# Match the LED structure supplied by the target kernel headers.
ifneq ($(shell grep -l INT3472_MAX_LEDS $(KDIR)/include/linux/platform_data/x86/int3472.h 2>/dev/null),)
int346f-y += led.o
else
int346f-y += led-legacy.o
endif


all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
