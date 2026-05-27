.. SPDX-License-Identifier: GPL-2.0+

Clock API
=========

.. kernel-doc:: include/clk.h
   :doc: Overview

Client API
----------

.. kernel-doc:: include/clk.h
   :internal:

Driver API
----------

.. kernel-doc:: include/clk-uclass.h
   :internal:

Common Clock Framework
----------------------

U-Boot includes optional support for the `Linux Common Clock Framework <https://www.kernel.org/doc/Documentation/clk.txt>`_
in two forms:

IMX U-Boot CCF
^^^^^^^^^^^^^^

Basic support for managing the clock tree, using per-clock ops, propagating rate change requests, and using common
divider/mux/gate clocks is provided by the earlier ``uCCF`` or ``U-Boot CCF``, it uses ``UCLASS_CLK`` and the associated
:c:struct:`clk_ops_uboot`. The clock tree itself is tracked through U-Boot driver model as each clock has a single ``udevice``.

This is described in more detail in `The IMX CCF document <https://source.denx.de/u-boot/u-boot/-/blob/master/doc/imx/clk/ccf.txt>`_.

CCF Full Port
^^^^^^^^^^^^^

The second version of CCF (*CCF Full*) focuses on maintaining compatibility with the Linux API. It introduces the
typical ``struct clk_hw`` and other expected functionality. Clocks are tracked by the CCF core rather than through the driver model. This
port uses ``struct clk_ops`` and allows drivers of any uclass to register clocks. This greatly simplifies the process of porting
devices that like some phy's which also provide clocks, as it removes the need to declare an additional ``U_BOOT_DRIVER`` for uclass clk.

:doc:`More information, recommendations, and limitations of CCF Full </develop/ccf>`
