// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright (C) 2020 Cortina-Access
 * Author: Jway Lin <jway.lin@cortina-access.com>
 *
 */

#include <dm.h>
#include <errno.h>
#include <led.h>
#include <log.h>
#include <pwm.h>
#include <asm/io.h>
#include <dm/lists.h>
#include <linux/bitops.h>
#include <linux/time.h>
#include <power/pmic.h>

#define REG_TYPE		0x4
#define REG_SUBTYPE		0x5

#define REG_SIZE_PER_LPG	0x100

// LED module defines, base adddress should also be base address from DTS
#define LED_BASE_ADDR			0xEF00
#define TRILED_REG_EN_CTL		0x46
#define TRILED_TYPE			0x19
#define TRILED_SUBTYPE_LED3H0L12	0x02

/* LED_CONFIG structures */
struct qpnp_tri_led_priv {
	struct udevice *pmic;
	struct udevice *pwm;

	u8 mask;
	u32 base;
	u16 period;
	u16 channel_index;
	enum led_state_t state;
};

extern int qcom_lpg_pwm_output_type_set(struct udevice *dev, u32 channel_id,
				 u32 output_type);

static enum led_state_t qcom_tri_led_get_state(struct udevice *dev)
{
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);

	return priv->state;
}

static int qcom_tri_led_set_period(struct udevice *dev, int period_ns)
{
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);

	priv->period = period_ns;

	return 0;
}

// TODO Make this support blinking fast and slow
static int qcom_tri_led_set_state(struct udevice *dev, enum led_state_t state)
{
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);
	uint period_ns = 0;
	uint duty_ns = 0;

	if ( priv == NULL || priv->pmic == NULL )
		return -1;

	if (state == LEDST_BLINK || LEDST_ON)
		duty_ns = USEC_PER_SEC;

	if (state == LEDST_BLINK)
		period_ns = USEC_PER_SEC;

	if (state == LEDST_ON || state == LEDST_BLINK) {
		pwm_set_config(priv->pwm, priv->channel_index, period_ns, duty_ns);
		pwm_set_enable(priv->pwm, priv->channel_index, !!duty_ns);

		pmic_clrsetbits(priv->pmic, LED_BASE_ADDR + TRILED_REG_EN_CTL,
				priv->mask, priv->mask);
	} else {
		pmic_clrsetbits(priv->pmic, LED_BASE_ADDR + TRILED_REG_EN_CTL,
				priv->mask, 0x00);
	}

	priv->state = state;

	return 0;
}

static const struct led_ops qcom_tri_led_ops = {
	.get_state = qcom_tri_led_get_state,
	.set_state = qcom_tri_led_set_state,
	.set_period = qcom_tri_led_set_period,
};

static int qcom_tri_led_probe(struct udevice *dev)
{
	struct led_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	int ret;
	u64 base;
	u32 mask;

	/* Ignore the top-level pon node */
	if (!uc_plat->label)
		return 0;

	/* the led nodes are children of the "tri-led" node, get the
	 * PMIC device to use in pmic_reg_* calls.
	 */
	priv->pmic = dev->parent->parent;

	ret = dev_read_phandle_with_args(dev, "pwms", "#pwm-cells", 0,
					 0, &args);
	if (ret < 0) {
		printf("failed to get pwm args for led: %s ret: %d\n", dev->name, ret);
	}

	priv->channel_index = args.args[0];
	priv->period = args.args[1];

	ret = uclass_get_device_by_ofnode(UCLASS_PWM, args.node, &priv->pwm);
	if (ret < 0) {
		printf("failed to get pwm for led: %s ret: %d\n", dev->name, ret);
		return -EINVAL;
	}

	/* Get the address of the parent */
	base = dev_read_addr(dev->parent);
	if (base == FDT_ADDR_T_NONE) {
		printf("%s: Can't find address\n", dev->name);
		return -EINVAL;
	}

	priv->base = base;

	ret = dev_read_u32(dev, "bit-mask", &mask);
	if (ret != 0) 
	{
		printf("%s: Provide bit-mask property for each LED\n", dev->name);
		return -EINVAL;
	}
	priv->mask = (u8)mask;

	/* Do a sanity check */
	ret = pmic_reg_read(priv->pmic, priv->base + REG_TYPE);
	if (ret != TRILED_TYPE) {
		printf("%s: unexpected PMIC function type %d\n",
			dev->name, ret);
		return -ENXIO;
	}

	ret = pmic_reg_read(priv->pmic, priv->base + REG_SUBTYPE);
	if (ret != TRILED_SUBTYPE_LED3H0L12) {
		printf("%s: unexpected PMIC function sub type %d\n",
			dev->name, ret);
		return -ENXIO;
	}

    return 0;
}

static int qcom_tri_led_bind(struct udevice *parent)
{
	struct udevice *dev;
	ofnode node;
	int ret;

	dev_for_each_subnode(node, parent) {
		struct led_uc_plat *uc_plat;
		// const char *label = NULL;
		// TODO Maybe get a label?

		if (!ofnode_is_enabled(node)) {
			continue;
		}

		ret = device_bind_driver_to_node(parent, "qcom-led",
						 ofnode_get_name(node),
						 node, &dev);
		if (ret) {
			printf("led_qcom Failed to bind! %d\n", ret);
			return ret;
		}
		uc_plat = dev_get_uclass_plat(dev);
		uc_plat->label = ofnode_get_name(node);
	}

	return 0;
}

static const struct udevice_id qcom_tri_led_ids[] = {
	{ .compatible = "qcom,qcom-led" },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(qcom_led) = {
	.name = "qcom-led",
	.id = UCLASS_LED,
	.of_match = qcom_tri_led_ids,
	.bind = qcom_tri_led_bind,
	.probe = qcom_tri_led_probe,
	.priv_auto	= sizeof(struct qpnp_tri_led_priv),
	.ops = &qcom_tri_led_ops,
};
