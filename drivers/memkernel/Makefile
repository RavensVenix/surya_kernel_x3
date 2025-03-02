ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
	obj-$(CONFIG_MEMKERNEL)  := memkernel.o
	memkernel-y := memory.o process.o entry.o
else
# normal makefile
default:
	# Assume kernel source is at KDIR or fail
	ifeq ($(KDIR),)
		$(error KDIR is not set. Please define KDIR to point to the kernel source directory.)
	endif
	make -C $(KDIR) M=$(PWD) modules
clean:
	rm -f *.ko *.o *.mod.o *.mod.c *.symvers .*.cmd
endif