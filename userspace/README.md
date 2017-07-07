This directory contains a pure userspace implementation.
Carrier frequency is hardcoded to 40.685 MHz and sample rate to ~1500 baud.
It also takes care of bit encoding:
```
    0 = 101
    1 = 100
```
Usage: 
```
door <switches state on remote>
```
