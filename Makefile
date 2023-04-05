export ARCH:=arm64
export CROSS_COMPILE:=aarch64-linux-

DTBO = dummyvfs.dtbo
DTBS = dummyvfs-overlay.dts

obj-m+=dummyvfs.o

KDIR?=/home/oleksii/linx/raspkrn/output/build/linux-custom
PWD:=$(shell pwd)

all : module

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

.PHONY : clean

clean:
	rm -rf *.o *~core .depend .*.cmd *.ko *.mod.c .tmp_versions *.mod
	rm -rf ./arch ./include ./kernel *.symvers *.order
	rm -rf $(DTBO)

# END OF FILE
