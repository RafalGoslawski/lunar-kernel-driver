# Lunar 2-in-1 Mini PC EC Driver

## Overview

This is a Linux kernel HWMON driver for the Lunar 2-in-1 Mini PC, providing:

* Fan speed monitoring and control
* LED control for keyboard indicators (L1 and L2)

## Building & Installing

```bash
make clean
make
sudo make install
```

### Using DKMS

To install:

```bash
sudo make dkms
```

To remove:

```bash
sudo make dkms_clean
```

### Notes:

* The module does not provide a real version number, so `git describe --long`
  is used to create one. This means that anything that changes the git state
  will change the version. `make dkms_clean` should be run before making a
  commit or an update with `git pull` as the Makefile is currently unable to
  track the last installed version to replace it. If this doesn't happen, the
  old version will need to be manually removed from dkms, before installing
  the updated module.
  Something like `dkms remove -m lunar -v <old version> --all`, followed by
  `rm -rf /usr/src/lunar-<old version>`, should do.
  `dkms status -m lunar` can be used to list the installed versions.

## Features

* Read fan RPM via EC register values
* Control fan speed in automatic/manual modes
* Expose fan control and telemetry via `hwmon` interface
* LED brightness control for keyboard indicators through `leds` subsystem

## Architecture

The driver communicates with the EC using LPC IO ports:

* Configuration addresses at `0x4E` (address) and `0x4F` (data)
* Addressing EC RAM using custom protocols via `D2ADR/D2DAT` IO ports

## Modules

### EC RAM I/O

Handles low-level read/write to EC RAM using mutex-protected access:

* `ec_ram_read(uint16_t address)`
* `ec_ram_write(uint16_t address, uint8_t data)`

### Fan Control

* Data exposed via `hwmon` and `thermal_cooling_device` APIs
* PWM values scaled between EC range (0-184) and hwmon (0-200)
* Supports three modes:

  * Off
  * Manual (user-defined PWM)
  * Auto (firmware-controlled)

### LED Control

* Two LEDs (L1 and L2) controlled by writing to EC
* State maintained in memory due to lack of readable EC brightness register

## Interfaces

### HWMON sysfs

Exposes the following attributes:

* `/sys/class/hwmon/hwmonX/fan1_input` – current fan speed (RPM)
* `/sys/class/hwmon/hwmonX/pwm1` – fan speed setting (0-200)
* `/sys/class/hwmon/hwmonX/pwm1_enable` – fan mode:

  * 0 = Off (control off, speed 100%)
  * 1 = Manual
  * 2 = Automatic

### LED sysfs

Exposes standard LED class entries:

* `/sys/class/leds/L1/brightness` - (0 off, 1-255 on)
* `/sys/class/leds/L2/brightness` - (0 off, 1-255 on)

## DMI Match Table

Ensures driver only loads on supported hardware:

```c
static const struct dmi_system_id lunar_dmi_table[] __initconst = {
    LUNAR_DMI_MATCH_VND("SU", "ARB33P"),
    { }
};
```

## Register Map

| Register            | Address (hex) | Description                  |
| ------------------- | ------------- | ---------------------------- |
| PNPCFG\_ADDR        | 0x4E          | EC address port              |
| PNPCFG\_DATA        | 0x4F          | EC data port                 |
| D2ADR               | 0x2E          | EC indirect address register |
| D2DAT               | 0x2F          | EC indirect data register    |
| I2EC\_ADDR\_L       | 0x10          | EC RAM address low byte      |
| I2EC\_ADDR\_H       | 0x11          | EC RAM address high byte     |
| I2EC\_DATA          | 0x12          | EC RAM data access           |
| FAN\_TACHO\_ADDR\_H | 0x0218        | Fan speed high byte          |
| FAN\_TACHO\_ADDR\_L | 0x0219        | Fan speed low byte           |
| FAN\_PWM\_ADDR      | 0x1809        | PWM duty cycle               |
| FAN\_MODE\_ADDR     | 0x0F02        | Fan control mode             |
| L1 LED Address      | 0x04A0        | L1 keyboard LED control      |
| L2 LED Address      | 0x04A1        | L2 keyboard LED control      |

## License

GPL-2.0-or-later

## Author

Rafal Goslawski <rafal.goslawski@gmail.com>
