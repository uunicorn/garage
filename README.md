# Garage
This repo contains a kernel driver which allows you to use your RPi as a radio transmitter.
It is heavily influenced by RpiTx userspace app, but only supports CW (on/off) modulation.

I built it for the only purpose: to open my garage door using my smart phone. 
I've got an Apache server running on my RPi with a single page and a cgi-bin script similar 
to [this](test.sh). This server is only visible within my Wifi network, so Wifi password is 
effectively used to restricts access to this resource.

## Hardware
Connect a piece of wire to GPIO18 pin. Ideally ~2m long, but ~20cm works as well. This is your antenna.

## Building
Instructions may vary depending on your RPi version.

### Install cross compile toolchain
e.g gcc-linaro-arm-linux-gnueabihf-raspbian-x64

### Firmware revision
```
root@pi:~# zcat /usr/share/doc/raspberrypi-bootloader/changelog.Debian.gz | grep 'firmware as of' | head -1 | sed 's/.*firmware as of //'
2a329e0c7d8ea19c085bac5633aa4fccee0f21be
```

This is your firmware revision. 

### Kernel sources
Use your firmware revision to the kernel sources revision: 
```
$ curl https://raw.githubusercontent.com/raspberrypi/firmware/2a329e0c7d8ea19c085bac5633aa4fccee0f21be/extra/git_hash
bc1669c846b629cface0aaa367afb2b9c6226faf
```
Checkout this revision from https://github.com/raspberrypi/linux.git

### .config
```
root@pi:~# modprobe configs
root@pi:~# gunzip < /proc/config.gz > .config
```
Download this .config to your buildbox and put it in the root of your kernel source.

### Symvers
Use the firmware revision to get the symvers. In the root of kernel source tree run: 
```
curl https://raw.githubusercontent.com/raspberrypi/firmware/2a329e0c7d8ea19c085bac5633aa4fccee0f21be/extra/Module7.symvers > Module.symvers
```

### Set up your shell environment
It's useful to have a script like this. 
```
export PATH=$PATH:~/tmp/rpi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/bin/
export CROSS_COMPILE=arm-linux-gnueabihf-
export ARCH=arm
export KSRC=~/tmp/rpi/linux
```
You will need to source it each time you want to rebuild the module.

### Preparing to build modules
```
$ make modules_prepare
```
### Building this module
Given you've set up your environment variables like in the steps above, checkout this repo and run
```
make
```
## Running
Upload built garage-door.ko to RPi and run:
```
insmod garage-door.ko
```
Repo contains a [test.sh](test.sh) script which contains an example usage.
