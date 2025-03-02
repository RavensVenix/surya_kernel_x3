#!/bin/sh

echo " " | tee -a ../Makefile
echo -n 'obj-$(CONFIG_RWMEM)		+= rwmem/' | tee -a ../Makefile

sed -i "/endmenu/i\source \"drivers/rwmem/Kconfig\"" ../Kconfig
sed -i "/endmenu/i\ " ../Kconfig