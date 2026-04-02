# SPDX-License-Identifier: GPL-2.0+
#
# Copyright (c) 2026 Linaro Ltd.
#
# Full Linux CCF support
#

obj-y += clk.o \
	clk-conf.o \
	clk-divider.o \
	clk-gate.o \
	clk-mux.o \
	clk-fixed-factor.o \
	clk-fixed-rate.o

obj-$(CONFIG_CLK_COMPOSITE_CCF) += clk-composite.o

obj-$(CONFIG_SANDBOX_CLK_CCF) += clk_sandbox_ccf_full.o
obj-$(CONFIG_SANDBOX) += clk_sandbox.o
