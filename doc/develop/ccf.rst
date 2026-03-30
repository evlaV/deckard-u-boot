.. SPDX-License-Identifier: GPL-2.0+

Linux Common Clock Framework
============================

U-Boot includes an API-compatible port of the Linux CCF. This replaces the traditional
``UCLASS_CLK`` and aims to simplify platform bringup by allowing straightforward reuse
of existing Linux drivers.

As a result, there are some important limitations when it comes to using CCF:

1. It is NOT possible to combine CCF clocks with ``UCLASS_CLK`` since neither is aware
   of the other. When CCF is enabled, the CCF drivers will be used for fixed clocks and
   the typical ``clk_*`` functions will only work with CCF clocks, essentially making
   clock drivers that use ``UCLASS_CLK`` unusable.
2. CCF clocks can't currently be used pre-relocation. CCF expects to manage the entire
   clock tree (and must in order to properly calculate clock rates) so while it would
   be possible to probe only the minimum necessary clocks pre-reloc that can still be
   a lot of associated clock provider logic.
   There isn't today an ideal solution for this other than minimising/removing the use
   of driver pre-relocation and relying on earlier bootloader stages to set up the UART.
3. Similarly, CCF currently isn't supported in SPL although it could be enabled if there
   is enough malloc space available and dcache is enabled. It may be desirable to add
   a mechanism for only supporting a subset of clocks and providing "stubs" for parent
   clocks that are already configured to a good known rate.

Porting Linux Clock Drivers
---------------------------

By remaining as close as possible to the Linux API, the process of porting Linux clock
drivers is fairly straightforward. The Qualcomm CCF drivers in ``drivers/clk/qcom/ccf/``
can be used as an example.

Predominantly, ``UCLASS_CLK`` should NOT be used, instead clock controller drivers should
use ``UCLASS_NOP`` or another appropriate uclass.
