# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>

ifdef CONFIG_SPL_BUILD
obj-y := deckard_spl.o deckard_battery.o smb1510.o pm8550b.o pm8550vs.o pmk8550.o
else
obj-y := deckard.o deckard_devices.o deckard_eeprom.o deckard_battery.o smb1510.o pm8550b.o pm8550vs.o pmk8550.o
endif

warn_flags := -Wextra -Werror -Wformat=2 \
	-Wno-unused-parameter -Wno-unused-function \
	-Wno-unused-variable -Wno-sign-compare

ccflags-y += $(warn_flags)
subdir-ccflags-y += $(warn_flags)