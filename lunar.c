/* SPDX-License-Identifier: GPL-2.0-or-later */

// HWMON driver for Lunar 2-in-1 mini PC
// Copyright (C) 2025 Rafal Goslawski <rafal.goslawski@gmail.com>

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/leds.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#ifndef LUNAR_DRIVER_VERSION
#define LUNAR_DRIVER_VERSION "<not provided>"
#endif

#define DRVNAME "lunar"

#define PNPCFG_ADDR 0x4E
#define PNPCFG_DATA 0x4F
#define D2ADR       0x2E
#define D2DAT       0x2F
#define I2EC_ADDR_L 0x10
#define I2EC_ADDR_H 0x11
#define I2EC_DATA   0x12

#define FAN_TACHO_ADDR_H  0x0218
#define FAN_TACHO_ADDR_L  0x0219
#define FAN_PWM_ADDR      0x1809
#define FAN_MODE_ADDR     0x0F02
#define FAN_MODE_AUTO     0x00
#define FAN_MODE_MANUAL   0x6C
#define FAN_PWM_REG_MAX   0xB8
#define FAN_SPEED_MAX     7200
#define HWMON_PWM_MAX     200

#define LUNAR_NLEDS       2
#define LUNAR_LED_L1_ADDR 0x04A0
#define LUNAR_LED_L2_ADDR 0x04A1
#define LUNAR_LED_OFF     0x80
#define LUNAR_LED_ON      0x81

DEFINE_MUTEX(ec_lock);

static inline void lunar_ec_write(uint8_t reg, uint8_t val)
{
	outb(D2ADR, PNPCFG_ADDR);
	outb(reg, PNPCFG_DATA);
	outb(D2DAT, PNPCFG_ADDR);
	outb(val, PNPCFG_DATA);
}

static inline uint8_t lunar_ec_read(uint8_t reg)
{
	outb(D2ADR, PNPCFG_ADDR);
	outb(reg, PNPCFG_DATA);
	outb(D2DAT, PNPCFG_ADDR);
	return inb(PNPCFG_DATA);
}

static void ec_select_address(uint16_t addr)
{
	lunar_ec_write(I2EC_ADDR_H, addr >> 8);
	lunar_ec_write(I2EC_ADDR_L, addr & 0xFF);
}

static void ec_ram_write(uint16_t addr, uint8_t data)
{
	mutex_lock(&ec_lock);
	ec_select_address(addr);
	lunar_ec_write(I2EC_DATA, data);
	mutex_unlock(&ec_lock);
}

static uint8_t ec_ram_read(uint16_t addr)
{
	uint8_t result;
	mutex_lock(&ec_lock);
	ec_select_address(addr);
	result = lunar_ec_read(I2EC_DATA);
	mutex_unlock(&ec_lock);
	return result;
}

enum pwm_mode { PWM_OFF, PWM_MANUAL, PWM_AUTO };

struct lunar_fan {
	struct device *dev;
	enum pwm_mode pwm_mode;
	uint8_t pwm_target;
};

#define TO_HWMON(pwm) mult_frac(pwm, HWMON_PWM_MAX, FAN_PWM_REG_MAX)
#define TO_REG(pwm)   mult_frac(pwm, FAN_PWM_REG_MAX, HWMON_PWM_MAX)

static int lunar_fan_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct lunar_fan *fan = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		*val = ((uint16_t)ec_ram_read(FAN_TACHO_ADDR_H) << 8) |
			ec_ram_read(FAN_TACHO_ADDR_L);
		return 0;
	case hwmon_pwm:
		if (attr == hwmon_pwm_enable)
			*val = fan->pwm_mode;
		else if (attr == hwmon_pwm_input)
			*val = TO_HWMON(ec_ram_read(FAN_PWM_ADDR));
		else
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static void lunar_fan_set_speed(uint8_t speed)
{
	ec_ram_write(FAN_PWM_ADDR, TO_REG(min_t(uint8_t, speed, HWMON_PWM_MAX)));
}

static int lunar_fan_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct lunar_fan *fan = dev_get_drvdata(dev);

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_enable:
		if (val < 0 || val > 2)
			return -EOPNOTSUPP;
		fan->pwm_mode = val;
		ec_ram_write(FAN_MODE_ADDR, val == PWM_AUTO ? FAN_MODE_AUTO : FAN_MODE_MANUAL);
		if (val == PWM_OFF)
			lunar_fan_set_speed(HWMON_PWM_MAX);
		else if (val == PWM_MANUAL)
			lunar_fan_set_speed(fan->pwm_target);
		return 0;
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		fan->pwm_target = val;
		if (fan->pwm_mode == PWM_MANUAL)
			lunar_fan_set_speed(val);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t lunar_fan_is_visible(const void *data,
				 enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	if (type == hwmon_fan && attr == hwmon_fan_input)
		return 0444;
	if (type == hwmon_pwm && (attr == hwmon_pwm_enable || attr == hwmon_pwm_input))
		return 0644;
	return 0;
}

static const struct hwmon_channel_info * const lunar_fan_channels[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops lunar_fan_hwmon_ops = {
	.is_visible = lunar_fan_is_visible,
	.read = lunar_fan_read,
	.write = lunar_fan_write,
};

static const struct hwmon_chip_info lunar_fan_hwmon_chip_info = {
	.ops = &lunar_fan_hwmon_ops,
	.info = lunar_fan_channels,
};

struct lunar_led {
	uint16_t address;
	uint8_t brightness;
	struct led_classdev led_cdev;
};

#define led_cdev_to_lunar_led(c) container_of(c, struct lunar_led, led_cdev)

static enum led_brightness lunar_led_get(struct led_classdev *led_cdev)
{
	return led_cdev_to_lunar_led(led_cdev)->brightness;
}

static void lunar_led_set(struct led_classdev *led_cdev, enum led_brightness brightness)
{
	struct lunar_led *led = led_cdev_to_lunar_led(led_cdev);
	led->brightness = brightness;
	ec_ram_write(led->address, brightness == LED_OFF ? LUNAR_LED_OFF : LUNAR_LED_ON);
}

#define LUNAR_DMI_MATCH_VND(vendor, name) \
	{ .matches = { \
		DMI_EXACT_MATCH(DMI_BOARD_VENDOR, vendor), \
		DMI_EXACT_MATCH(DMI_BOARD_NAME, name), \
	} }

static const struct dmi_system_id lunar_dmi_table[] __initconst = {
	LUNAR_DMI_MATCH_VND("SU", "ARB33P"),
	{}
};

static const char *const led_names[LUNAR_NLEDS] = {"L1", "L2"};
static const uint16_t led_addrs[LUNAR_NLEDS] = {LUNAR_LED_L1_ADDR, LUNAR_LED_L2_ADDR};

static int lunar_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lunar_fan *fan;
	struct lunar_led *leds;
	struct device *hwmon_dev;
	int i, ret;

	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	fan->dev = dev;
	fan->pwm_target = HWMON_PWM_MAX;
	fan->pwm_mode = PWM_AUTO;
	platform_set_drvdata(pdev, fan);

	ec_ram_write(FAN_MODE_ADDR, FAN_MODE_AUTO);

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "lunar_fan",
						 fan, &lunar_fan_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	leds = devm_kcalloc(dev, LUNAR_NLEDS, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	for (i = 0; i < LUNAR_NLEDS; i++) {
		leds[i].address = led_addrs[i];
		leds[i].brightness = 0;
		leds[i].led_cdev.name = led_names[i];
		leds[i].led_cdev.brightness_get = lunar_led_get;
		leds[i].led_cdev.brightness_set = lunar_led_set;
		ret = devm_led_classdev_register(dev, &leds[i].led_cdev);
		if (ret) {
			dev_err(dev, "Failed to register LED %s\n", led_names[i]);
			return ret;
		}
	}

	return 0;
}

static struct platform_driver lunar_ec_platform_driver = {
	.driver = {
		.name = "lunar-ec",
		.bus = &platform_bus_type,
	},
	.probe = lunar_ec_probe,
};

static struct platform_device *lunar_ec_platform_device;

static int __init lunar_ec_init(void)
{
	lunar_ec_platform_device = platform_create_bundle(&lunar_ec_platform_driver,
						  lunar_ec_probe, NULL, 0, NULL, 0);
	if (IS_ERR(lunar_ec_platform_device))
		return PTR_ERR(lunar_ec_platform_device);
	return 0;
}

static void __exit lunar_ec_exit(void)
{
	ec_ram_write(FAN_MODE_ADDR, PWM_AUTO);
	platform_device_unregister(lunar_ec_platform_device);
	platform_driver_unregister(&lunar_ec_platform_driver);
}

module_init(lunar_ec_init);
module_exit(lunar_ec_exit);

MODULE_AUTHOR("Rafal Goslawski <rafal.goslawski@gmail.com>");
MODULE_DESCRIPTION("Lunar 2-in-1 mini PC Sensors Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(LUNAR_DRIVER_VERSION);
MODULE_DEVICE_TABLE(dmi, lunar_dmi_table);
