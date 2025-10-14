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
