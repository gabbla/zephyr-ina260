# Zephyr driver for the INA260 current/voltage/power monitor

A Zephyr sensor driver for the Texas Instruments INA260 on the i2c bus.

## Add this module to west.yml

```
    - name: golioth-ina260-driver
      path: deps/modules/lib/ina260
      revision: v1.0.0
      url: https://github.com/golioth/zephyr-ina260.git
```

## Use the driver

Add an instance to devicetree

```
&i2c2 {
	ina260_ch0: ina260@40 {
		compatible = "ti,ina260";
		reg = <0x40>;
	};
	ina260_ch1: ina260@41 {
		compatible = "ti,ina260";
		reg = <0x41>;
	};
};

```

Include the header file

```
#include <zephyr/drivers/sensor/ina260.h>
```

## Runtime ALERT pin configuration

Enable trigger support in `prj.conf`:

```ini
CONFIG_SENSOR=y
CONFIG_INA260=y
CONFIG_INA260_TRIGGER=y
```

Connect ALERT to a GPIO and add `alert-gpios` to the sensor node:

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

&i2c2 {
	ina260_ch0: ina260@40 {
		compatible = "ti,ina260";
		reg = <0x40>;
		alert-gpios = <&gpio0 11 (GPIO_ACTIVE_LOW | GPIO_PULL_UP)>;
	};
};
```

ALERT is an active-low, open-drain output and needs a pull-up. With an
external pull-up, use `GPIO_ACTIVE_LOW` without `GPIO_PULL_UP`. Instances
without `alert-gpios` continue to support polling.

Configure the condition and limit with `sensor_attr_set()`, then enable the
pin and callback with `sensor_trigger_set()`:

```c
#include <zephyr/drivers/sensor/ina260.h>

static const struct device *const monitor =
	DEVICE_DT_GET(DT_NODELABEL(ina260_ch0));

/* Keep the trigger alive while its callback is registered. */
static const struct sensor_trigger current_alert = {
	.type = SENSOR_TRIG_THRESHOLD,
	.chan = SENSOR_CHAN_CURRENT,
};

static void on_alert(const struct device *dev,
		     const struct sensor_trigger *trigger)
{
	struct sensor_value current;

	if (sensor_sample_fetch(dev) == 0 &&
	    sensor_channel_get(dev, SENSOR_CHAN_CURRENT, &current) == 0) {
		/* Handle the alert; current is in amperes. */
	}
}

int enable_current_alert(void)
{
	const struct sensor_value limit = {.val1 = 2, .val2 = 500000}; /* 2.5 A */
	int err;

	if (!device_is_ready(monitor)) {
		return -ENODEV;
	}
	err = sensor_attr_set(monitor, SENSOR_CHAN_CURRENT,
			      SENSOR_ATTR_UPPER_THRESH, &limit);
	if (err) {
		return err;
	}
	return sensor_trigger_set(monitor, &current_alert, on_alert);
}
```

Supported threshold conditions:

| Condition | Channel | Attribute | Limit units / resolution |
| --- | --- | --- | --- |
| Over-current (OCL) | `SENSOR_CHAN_CURRENT` | `SENSOR_ATTR_UPPER_THRESH` | A / 1.25 mA |
| Under-current (UCL) | `SENSOR_CHAN_CURRENT` | `SENSOR_ATTR_LOWER_THRESH` | A / 1.25 mA |
| Bus over-voltage (BOL) | `SENSOR_CHAN_VOLTAGE` | `SENSOR_ATTR_UPPER_THRESH` | V / 1.25 mV |
| Bus under-voltage (BUL) | `SENSOR_CHAN_VOLTAGE` | `SENSOR_ATTR_LOWER_THRESH` | V / 1.25 mV |
| Over-power (POL) | `SENSOR_CHAN_POWER` | `SENSOR_ATTR_UPPER_THRESH` | W / 10 mW |

`sensor_value.val1` is the whole part and `val2` the fractional part in
millionths. Negative current thresholds are supported. Limits are truncated
toward zero to the register resolution; values outside the register range
return `-EINVAL`. Unsupported channel/attribute combinations return `-ENOTSUP`.

The INA260 has one shared threshold register, so only **one threshold condition**
can be configured at a time. Calling `sensor_attr_set()` again changes the
condition and limit at runtime. For example, switch from OCL to UCL while
keeping the callback registered:

```c
const struct sensor_value low_limit = {.val1 = 0, .val2 = 100000}; /* 0.1 A */
int err = sensor_attr_set(monitor, SENSOR_CHAN_CURRENT,
			  SENSOR_ATTR_LOWER_THRESH, &low_limit);
```

To change the monitored channel or switch between threshold and conversion-ready
alerts, first disable the callback with
`sensor_trigger_set(monitor, &current_alert, NULL)`, configure the new threshold
if needed, and register the new trigger. Changing the threshold channel while a
callback is registered returns `-EBUSY`. Registering a threshold trigger before
configuring a matching threshold returns `-EINVAL`.

Conversion-ready alerts use `SENSOR_TRIG_DATA_READY` with `SENSOR_CHAN_ALL`:

```c
static const struct sensor_trigger ready = {
	.type = SENSOR_TRIG_DATA_READY,
	.chan = SENSOR_CHAN_ALL,
};
int err = sensor_trigger_set(monitor, &ready, on_alert);
```

Callbacks run on Zephyr's system work queue, outside interrupt context, and may
call sensor APIs. Keep callbacks short. The driver uses latched alerts and reads
the Mask/Enable register to acknowledge the interrupt before invoking the
callback. Samples are not fetched automatically. A persistent threshold
violation can produce another alert after acknowledgement. Events may coalesce
while work is pending; conversion-ready callbacks are not a lossless sample
stream. Disabling a trigger does not wait for a callback already executing.
Configuration functions must be called from thread context. Check their return
values: an I²C failure during reconfiguration may leave alerts disabled; retry
configuration after resolving the bus error.

## Tests

Run the I²C/GPIO emulator tests from a configured Zephyr workspace:

```sh
west twister -T /path/to/zephyr-ina260/tests/alert -p native_sim/native/64 --inline-logs
```

The suite covers threshold modes and register scaling, runtime updates, deferred
callbacks and acknowledgement, conversion-ready alerts, I²C errors, optional
ALERT GPIOs, and polling with trigger support disabled.
