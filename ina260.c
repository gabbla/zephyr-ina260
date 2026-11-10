/*
 * Copyright (c) 2023 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT ti_ina260

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ina260, LOG_LEVEL_DBG);

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/drivers/gpio.h>

#include <zephyr/drivers/sensor/ina260.h>

#ifdef CONFIG_INA260_FAKE
__attribute__((weak))
#else
static
#endif
int ina260_reg_read(const struct device *dev,
		    uint8_t reg_addr,
		    uint16_t *reg_data)
{
	const struct ina260_device_config *cfg = dev->config;
	uint8_t rx_buf[2];
	int err;

	err = i2c_write_read_dt(&cfg->bus,
				&reg_addr, sizeof(reg_addr),
				rx_buf, sizeof(rx_buf));

	if (!err) {
		*reg_data = sys_get_be16(rx_buf);
	}

	return err;
}

static int ina260_sample_fetch(const struct device *dev,
			       enum sensor_channel chan)
{
	int err;
	uint16_t reading;

	struct ina260_data *data = dev->data;

	if (chan != SENSOR_CHAN_ALL &&
		chan != SENSOR_CHAN_VOLTAGE &&
		chan != SENSOR_CHAN_POWER &&
		chan != SENSOR_CHAN_CURRENT) {
		return -ENOTSUP;
	}

	if (chan == SENSOR_CHAN_ALL ||
		chan == SENSOR_CHAN_VOLTAGE) {

		err = ina260_reg_read(dev, INA260_REG_VOLTAGE, &reading);
		if (err) {
			LOG_ERR("Error reading bus voltage.");
			return err;
		}
		data->vol = (int16_t)reading;
	}

	if (chan == SENSOR_CHAN_ALL ||
		chan == SENSOR_CHAN_POWER)	{

		err = ina260_reg_read(dev, INA260_REG_POWER, &reading);
		if (err) {
			LOG_ERR("Error reading power register.");
			return err;
		}
		data->pow = (uint16_t)reading;
	}

	if (chan == SENSOR_CHAN_ALL ||
		chan == SENSOR_CHAN_CURRENT) {

		err = ina260_reg_read(dev, INA260_REG_CURRENT, &reading);
		if (err) {
			LOG_ERR("Error reading current register.");
			return err;
		}
		data->cur = (int16_t)reading;
	}
	return err;
}

static int ina260_channel_get(const struct device *dev,
			      enum sensor_channel chan,
			      struct sensor_value *val)
{
	int err;
	double calculated;

	struct ina260_data *data = dev->data;

	switch ((int16_t)chan) {
	case SENSOR_CHAN_VOLTAGE:
		calculated = ((double)data->vol * 125) / 100000;
		break;
	case SENSOR_CHAN_POWER:
		calculated = (double)data->pow / 100;
		break;
	case SENSOR_CHAN_CURRENT:
		calculated = ((double)data->cur * 125) / 100000;
		break;
	case SENSOR_CHAN_INA260_VOLTAGE_RAW:
		val->val1 = data->vol;
		return 0;
	case SENSOR_CHAN_INA260_CURRENT_RAW:
		val->val1 = data->cur;
		return 0;
	case SENSOR_CHAN_INA260_POWER_RAW:
		val->val1 = data->pow;
		return 0;
	default:
		return -ENOTSUP;
	}

	err = sensor_value_from_double(val, calculated);
	return err;
}

#ifdef CONFIG_INA260_TRIGGER
#define INA260_ALERT_OCL BIT(15)
#define INA260_ALERT_UCL BIT(14)
#define INA260_ALERT_BOL BIT(13)
#define INA260_ALERT_BUL BIT(12)
#define INA260_ALERT_POL BIT(11)
#define INA260_ALERT_CNVR BIT(10)
#define INA260_ALERT_AFF BIT(4)
#define INA260_ALERT_CVRF BIT(3)
#define INA260_ALERT_LEN BIT(0)

static int ina260_reg_write(const struct device *dev, uint8_t reg, uint16_t value)
{
	const struct ina260_device_config *cfg = dev->config;
	uint8_t buf[3] = {reg};

	sys_put_be16(value, &buf[1]);
	return i2c_write_dt(&cfg->bus, buf, sizeof(buf));
}

static int ina260_attr_set(const struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr, const struct sensor_value *val)
{
	const struct ina260_device_config *cfg = dev->config;
	struct ina260_data *data = dev->data;
	int64_t micro, raw;
	uint16_t mask;
	int err;

	if (!cfg->alert_gpio.port) {
		return -ENOTSUP;
	}
	if (attr != SENSOR_ATTR_UPPER_THRESH && attr != SENSOR_ATTR_LOWER_THRESH) {
		return -ENOTSUP;
	}
	switch (chan) {
	case SENSOR_CHAN_CURRENT:
		mask = attr == SENSOR_ATTR_UPPER_THRESH ? INA260_ALERT_OCL : INA260_ALERT_UCL;
		break;
	case SENSOR_CHAN_VOLTAGE:
		mask = attr == SENSOR_ATTR_UPPER_THRESH ? INA260_ALERT_BOL : INA260_ALERT_BUL;
		break;
	case SENSOR_CHAN_POWER:
		if (attr != SENSOR_ATTR_UPPER_THRESH) {
			return -ENOTSUP;
		}
		mask = INA260_ALERT_POL;
		break;
	default:
		return -ENOTSUP;
	}
	micro = (int64_t)val->val1 * 1000000 + val->val2;
	if ((chan == SENSOR_CHAN_CURRENT && (micro < -40960000 || micro > 40958750)) ||
	    (chan == SENSOR_CHAN_VOLTAGE && (micro < 0 || micro > 81918750)) ||
	    (chan == SENSOR_CHAN_POWER && (micro < 0 || micro > 655350000))) {
		return -EINVAL;
	}
	raw = micro / (chan == SENSOR_CHAN_POWER ? 10000 : 1250);
	k_mutex_lock(&data->alert_lock, K_FOREVER);
	if (data->handler && (data->trigger->type != SENSOR_TRIG_THRESHOLD ||
			      data->trigger->chan != chan)) {
		err = -EBUSY;
		goto out;
	}
	/* Disable the old condition before replacing the shared limit. */
	err = ina260_reg_write(dev, INA260_REG_MASK_ENABLE, 0);
	if (err) {
		goto out;
	}
	err = ina260_reg_write(dev, INA260_REG_ALERT_LIMIT, (uint16_t)raw);
	if (err) {
		goto out;
	}
	data->alert_mask = mask;
	data->alert_channel = chan;
	if (data->handler) {
		err = ina260_reg_write(dev, INA260_REG_MASK_ENABLE, mask | INA260_ALERT_LEN);
	}
out:
	k_mutex_unlock(&data->alert_lock);
	return err;
}

static int ina260_trigger_set(const struct device *dev, const struct sensor_trigger *trig,
			      sensor_trigger_handler_t handler)
{
	const struct ina260_device_config *cfg = dev->config;
	struct ina260_data *data = dev->data;
	uint16_t mask;
	int err;

	if (!cfg->alert_gpio.port ||
	    (trig->type != SENSOR_TRIG_THRESHOLD && trig->type != SENSOR_TRIG_DATA_READY)) {
		return -ENOTSUP;
	}
	if (trig->type == SENSOR_TRIG_DATA_READY && trig->chan != SENSOR_CHAN_ALL) {
		return -ENOTSUP;
	}
	k_mutex_lock(&data->alert_lock, K_FOREVER);
	if (handler && trig->type == SENSOR_TRIG_THRESHOLD &&
	    (!data->alert_mask || trig->chan != data->alert_channel)) {
		err = -EINVAL;
		goto out;
	}
	err = ina260_reg_write(dev, INA260_REG_MASK_ENABLE, 0);
	if (err) {
		goto out;
	}
	data->handler = NULL;
	err = gpio_pin_interrupt_configure_dt(&cfg->alert_gpio,
					     handler ? GPIO_INT_EDGE_TO_ACTIVE : GPIO_INT_DISABLE);
	if (err || !handler) {
		goto out;
	}
	mask = trig->type == SENSOR_TRIG_DATA_READY ? INA260_ALERT_CNVR : data->alert_mask;
	data->trigger = trig;
	data->handler = handler;
	err = ina260_reg_write(dev, INA260_REG_MASK_ENABLE, mask | INA260_ALERT_LEN);
	if (err) {
		data->handler = NULL;
		(void)gpio_pin_interrupt_configure_dt(&cfg->alert_gpio, GPIO_INT_DISABLE);
	}
out:
	k_mutex_unlock(&data->alert_lock);
	return err;
}

static void ina260_alert_work(struct k_work *work)
{
	struct ina260_data *data = CONTAINER_OF(work, struct ina260_data, alert_work);
	sensor_trigger_handler_t handler = NULL;
	const struct sensor_trigger *trigger = NULL;
	uint16_t status;
	int err;

	k_mutex_lock(&data->alert_lock, K_FOREVER);
	/* Reading Mask/Enable acknowledges and releases the latched ALERT pin. */
	err = ina260_reg_read(data->dev, INA260_REG_MASK_ENABLE, &status);
	if (!err && data->handler) {
		trigger = data->trigger;
		if (status & (trigger->type == SENSOR_TRIG_DATA_READY ?
			      INA260_ALERT_CVRF : INA260_ALERT_AFF)) {
			handler = data->handler;
		}
	}
	k_mutex_unlock(&data->alert_lock);
	if (err) {
		LOG_ERR("Error acknowledging alert: %d", err);
	}
	if (handler) {
		handler(data->dev, trigger);
	}
}

static void ina260_gpio_callback(const struct device *port, struct gpio_callback *cb,
				 uint32_t pins)
{
	struct ina260_data *data = CONTAINER_OF(cb, struct ina260_data, alert_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	k_work_submit(&data->alert_work);
}

static int ina260_alert_init(const struct device *dev)
{
	const struct ina260_device_config *cfg = dev->config;
	struct ina260_data *data = dev->data;
	int err;

	data->dev = dev;
	k_mutex_init(&data->alert_lock);
	k_work_init(&data->alert_work, ina260_alert_work);
	if (!cfg->alert_gpio.port) {
		return 0;
	}
	if (!gpio_is_ready_dt(&cfg->alert_gpio)) {
		return -ENODEV;
	}
	if (!(cfg->alert_gpio.dt_flags & GPIO_ACTIVE_LOW)) {
		return -EINVAL;
	}
	err = gpio_pin_configure_dt(&cfg->alert_gpio, GPIO_INPUT);
	if (err) {
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(&cfg->alert_gpio, GPIO_INT_DISABLE);
	if (err) {
		return err;
	}
	err = ina260_reg_write(dev, INA260_REG_MASK_ENABLE, 0);
	if (err) {
		return err;
	}
	gpio_init_callback(&data->alert_cb, ina260_gpio_callback, BIT(cfg->alert_gpio.pin));
	return gpio_add_callback(cfg->alert_gpio.port, &data->alert_cb);
}
#endif

static int ina260_init(const struct device *dev)
{
	const struct ina260_device_config *config = dev->config;

	if (!device_is_ready(config->bus.bus)) {
		LOG_ERR("Bus device is not ready");
		return -ENODEV;
	}

#ifdef CONFIG_INA260_TRIGGER
	return ina260_alert_init(dev);
#else
	return 0;
#endif
}

static const struct sensor_driver_api ina260_api = {
	.sample_fetch = ina260_sample_fetch,
	.channel_get = ina260_channel_get,
#ifdef CONFIG_INA260_TRIGGER
	.attr_set = ina260_attr_set,
	.trigger_set = ina260_trigger_set,
#endif
};

#ifdef CONFIG_INA260_TRIGGER
#define INA260_ALERT_CONFIG(n) \
	.alert_gpio = GPIO_DT_SPEC_INST_GET_OR(n, alert_gpios, {0}),
#else
#define INA260_ALERT_CONFIG(n)
#endif

#define INA260_INIT(n)									\
	static struct ina260_data ina260_data_##n;					\
											\
	static const struct ina260_device_config ina260_device_config_##n = {		\
		.bus = I2C_DT_SPEC_INST_GET(n),						\
		INA260_ALERT_CONFIG(n)							\
	};										\
											\
	SENSOR_DEVICE_DT_INST_DEFINE(n, ina260_init, NULL,				\
			      &ina260_data_##n, &ina260_device_config_##n, POST_KERNEL,	\
			      CONFIG_SENSOR_INIT_PRIORITY, &ina260_api);

DT_INST_FOREACH_STATUS_OKAY(INA260_INIT)
