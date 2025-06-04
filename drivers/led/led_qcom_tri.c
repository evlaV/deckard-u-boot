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
#include <asm/io.h>
#include <dm/lists.h>
#include <linux/bitops.h>
#include <power/pmic.h>

#define REG_TYPE		0x4
#define REG_SUBTYPE		0x5

// HACK default setup for blue LED only 
// PWM module is in separate address space, same PMIC
#define REG_LPG_PWM_SIZE_CLK		0x41
#define REG_LPG_PWM_FREQ_PREDIV_CLK	0x42
#define REG_LPG_PWM_TYPE_CONFIG		0x43
#define REG_LPG_PWM_VALUE_LSB		0x44
#define REG_LPG_PWM_VALUE_MSB		0x45
#define REG_LPG_ENABLE_CONTROL		0x46
#define REG_LPG_PWM_SYNC            0x47

// LED module defines, base adddress should also be base address from DTS
#define LED_BASE_ADDR              0xEF00
#define TRILED_REG_EN_CTL 0x46
#define TRILED_TYPE 0x19
#define TRILED_SUBTYPE_LED3H0L12 0x02

/* LED_CONFIG structures */
struct qpnp_tri_led_priv {
    struct udevice *pmic;
	
	u32 base;
	u8 mask;
	u16 pwm_addr;
};

static enum led_state_t qcom_tri_led_get_state(struct udevice *dev)
{
	enum led_state_t state = LEDST_OFF;

	return state;
}

// TODO Make this support blinking fast and slow
// TODO Also it seems to hold state on a reboot from whatever linux was doing... probable need to wipe that out
static int qcom_tri_led_set_state(struct udevice *dev, enum led_state_t state)
{
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);

    if ( priv == NULL || priv->pmic == NULL ) {
        return -1;
    }

	// ret = pmic_reg_read(priv->pmic, priv->base + REG_TYPE);
	pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_PWM_SIZE_CLK, 0x03);

    if ( state == LEDST_ON ) {
        // PWM_FREQ = PWM_FREQ_CLK_SELECT / (2 ^ (PWM_SIZE)) * ( 2 ^ ( PWM_FREQ_EXPONENT ) * PWM_FREQ_PRE_DIVIDE )
        // PWM_FREQ_CLK_SELECT = 0x1 ( 1 kHz )
        // PWM_SIZE  = 0x1 ( 9 bit PWM )
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_PWM_SIZE_CLK, 0x03);

        // PWM_FREQ_EXPONENT = 0x001 ( 1 )
        // PWM_FREQ_PRE_DIVIDE = 0x00 ( 1 )
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_PWM_FREQ_PREDIV_CLK, 0x01);

        // PWM_VALUE_LSB = 0x0F ?? this sets the brightness
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_PWM_VALUE_LSB, 0x07);

        // set PWM sync so PWM_VALUE_LSB takes effect
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_PWM_SYNC, 0x01);

        // Enable PWM
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_ENABLE_CONTROL, 0x80);

        // LED Enable
        pmic_clrsetbits(priv->pmic, LED_BASE_ADDR + TRILED_REG_EN_CTL, priv->mask, priv->mask);
    } else {
        // Disable PWM
        pmic_reg_write(priv->pmic, priv->pwm_addr + REG_LPG_ENABLE_CONTROL, 0x00);

        // Disable LED
        pmic_clrsetbits(priv->pmic, LED_BASE_ADDR + TRILED_REG_EN_CTL, priv->mask, 0x00);
    }

	return 0;
}

static const struct led_ops qcom_tri_led_ops = {
	.get_state = qcom_tri_led_get_state,
	.set_state = qcom_tri_led_set_state,
};

static int qcom_tri_led_probe(struct udevice *dev)
{
	struct led_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct qpnp_tri_led_priv *priv = dev_get_priv(dev);
	//ofnode node = dev_ofnode(dev);
	int ret;
	u64 base;
	u32 mask;
	u32 pwm_addr;

	/* Ignore the top-level pon node */
	if (!uc_plat->label)
		return 0;

	/* the led nodes are children of the "tri-led" node, get the
	 * PMIC device to use in pmic_reg_* calls.
	 */
	priv->pmic = dev->parent->parent;

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

	ret = dev_read_u32(dev, "pwm-base-addr", &pwm_addr);
	if (ret != 0) 
	{
		printf("%s: Provide pwm-base-addr property for each LED\n", dev->name);
		return -EINVAL;
	}
	priv->pwm_addr = (u16)pwm_addr;

	/* Do a sanity check */
	ret = pmic_reg_read(priv->pmic, priv->base + REG_TYPE);
	if (ret != TRILED_TYPE) {
		printf("%s: unexpected PMIC function type %d\n", dev->name, ret);
		return -ENXIO;
	}
	ret = pmic_reg_read(priv->pmic, priv->base + REG_SUBTYPE);
	if (ret != TRILED_SUBTYPE_LED3H0L12) {
		printf("%s: unexpected PMIC function sub type %d\n", dev->name, ret);
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
			// printf("Failed to bind %s! %d\n", label, ret);
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
