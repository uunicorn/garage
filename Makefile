
obj-m += garage-door.o

all: modules

modules:
	$(MAKE) -C $(KSRC) M=$(shell pwd)  modules

clean:
	$(MAKE) -C $(KSRC) M=$(shell pwd)  clean
