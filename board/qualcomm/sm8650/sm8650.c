// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2022 Jonathan Marek <jonathan@marek.ca>
 */

#include <button.h>
#include <led.h>
#include <init.h>
#include <env.h>
#include <common.h>
#include <asm/system.h>
#include <asm/gpio.h>
#include <dm.h>

DECLARE_GLOBAL_DATA_PTR;

int dram_init(void)
{
	return fdtdec_setup_mem_size_base();
}

int dram_init_banksize(void)
{
	return fdtdec_setup_memory_banksize();
}

void reset_cpu(void)
{
    printf("custom reset\n");
	psci_system_reset();
}

int board_init(void)
{
	// usb_ether_init();
	return 0;
}

/* Check for vol- button - if pressed - stop autoboot */
int misc_init_r(void)
{
	struct udevice *btn,*led;
	int ret;
	enum button_state_t state;

	ret = button_get_by_label("vol_down", &btn);
	if (ret < 0) {
		printf("Couldn't find Vol- button!\n");
	} else {
        state = button_get_state(btn);
        if (state == BUTTON_ON) {
            env_set("BOOT_ORDER", "R");
            printf("Vol- button pressed - targeting rescue partition for boot.\n");
        }
    }

	ret = button_get_by_label("pwrkey", &btn);
	if (ret < 0) {
		printf("Couldn't find power button!\n");
	} else {
        state = button_get_state(btn);
        if (state == BUTTON_ON) {
            printf("PWR button pressed - do nothing.\n");
        }
    }

    ret  = led_get_by_label("blue", &led);
	if (ret < 0) {
		printf("Couldn't find blue led!\n");
	} else {
        led_set_state(led, LEDST_ON);
    };

	return 0;
}