# Comment/uncomment the following line to enable/disable debugging
#DEBUG = y


ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DPCI_INFO_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRA_CFLAGS += $(DEBFLAGS) -I$(LDDINC)

ifneq ($(KERNELRELEASE),)

obj-m	:= nfcsim.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules
clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) clean

endif




depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
