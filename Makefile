MODULE_NAME=garage-door

$(MODULE_NAME)-y += garage-driver.o garage-gpio.o garage-pwm.o garage-dma.o

obj-m := $(MODULE_NAME).o

all: modules

modules:
	$(MAKE) -C $(KSRC) M=$(shell pwd)  modules

clean:
	$(MAKE) -C $(KSRC) M=$(shell pwd)  clean
