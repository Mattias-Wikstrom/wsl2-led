This repository contains code for a Linux kernel module that is meant to be used with the Windows Subsystem for Linux version 2 (WSL2) rather than Linux environments in general. Once installed, this kernel module adds LED devices under /sys/class/leds.

There are no physical LEDs, but the Windows program in the LEDController folder will detect the data on whether a LED should be turned on/off and display the LEDs in the bottom-right corner of the Windows screen.
