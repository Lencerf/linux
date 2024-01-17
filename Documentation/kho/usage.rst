.. SPDX-License-Identifier: GPL-2.0-or-later

====================
Kexec Handover Usage
====================

Kexec HandOver (KHO) is a mechanism that allows Linux to preserve state -
arbitrary properties as well as memory locations - across kexec.

This document expects that you are familiar with the base KHO
:ref:`Documentation/kho/concepts.rst <concepts>`. If you have not read
them yet, please do so now.

Prerequisites
=============

KHO is available when the ``CONFIG_KEXEC_HANDOVER`` config option is set to y
at compile time. Every KHO producer may have its own config option that you
need to enable if you would like to preserve their respective state across
kexec.

To use KHO, please boot the kernel with the ``kho=on`` command line
parameter. You may use ``kho_scratch`` parameter to define size of the
scratch regions. For example ``kho_scratch=512M,512M`` will reserve a 512
MiB for a global scratch region and 512 MiB per NUMA node scratch regions
on boot.

Perform a KHO kexec
===================

First, before you perform a KHO kexec, you can optionally move the system into
the :ref:`Documentation/kho/concepts.rst <KHO finalization phase>` ::

  $ echo 1 > /sys/kernel/kho/finalize

After this command, the KHO FDT is available in ``/sys/kernel/kho/fdt``.

Next, load the target payload and kexec into it. It is important that you
use the ``-s`` parameter to use the in-kernel kexec file loader, as user
space kexec tooling currently has no support for KHO with the user space
based file loader ::

  # kexec -l Image --initrd=initrd -s
  # kexec -e

If you skipped finalization in the first step, `kexec -e` triggers finalization
automatically. The new kernel will boot up and contain some of the previous
kernel's state.

For example, if you used ``reserve_mem`` command line parameter to create
an early memory reservation, the new kernel will have that memory at the
same physical address as the old kernel.

Unfreeze KHO FDT data
================

You can move the system out of KHO finalization phase by calling ::

  $ echo 0 > /sys/kernel/kho/finalize

After this command, the KHO FDT is no longer available in
``/sys/kernel/kho/fdt``, and the states kept in KHO can be modified again.
