obj-m += sgp40_voc.o
sgp40_voc-objs := sgp40_voc_main.o sgp40_voc_algo.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
