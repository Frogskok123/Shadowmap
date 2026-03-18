obj-m += shadowmap.o

all:
\t$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

clean:
\t$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean
