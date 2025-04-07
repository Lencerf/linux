.. SPDX-License-Identifier: GPL-2.0-or-later

=======
KHO FDT
=======

KHO uses the flattened device tree (FDT) container format and libfdt
library to create and parse the data that is passed between the
kernels. The properties in KHO FDT are stored in native format and can
include preserved memory ranges or folios that KHO users need to preserve.
Interpreting the data in the preserved memory regions is the
responsibility of KHO users.

KHO nodes and properties
========================

Property ``preserved-memory-map``
---------------------------------

KHO saves a special property named ``preserved-memory-map`` under the root node.
This node contains the physical address to an in-memory structure for KHO to
preserve memory regions across kexec.

Property ``compatible``
-----------------------

The ``compatible`` property determines compatibility between the kernel
that created the KHO FDT and the kernel that attempts to load it.
If the kernel that loads the KHO FDT is not compatible with it, the entire
KHO process will be bypassed.

Property ``folio`` and ``fdt``
------------------------------

Generally, KHO users serialize their states into a buffer and instruct
KHO to preserve that buffer such that after kexec, the new kernel's
subsystems can recover their states from the preserved buffer.

Thus each KHO user can save the physical address of 1 folio to KHO root FDT,
which creates a child node with that name under the KHO root node and saves
the address in the child node's ``folio`` property. Specially, if KHO
users indicates the folio contains an FDT blob, KHO uses property name ``fdt``
instead.

Examples
========

The following example demonstrates KHO FDT that preserves two memory
regions created with ``reserve_mem`` kernel command line parameter::

  /dts-v1/;

  / {
  	compatible = "kho-v1";

	preserved-memory-map = <0x40be16 0x1000000>;

  	memblock {
		fdt = <0x1517 0x1000000>;
  	};
  };

where the ``memblock`` node contains a folio that is requested by the
subsystem memblock for preservation. The folio stores the following
FDT::

  /dts-v1/;

  / {
  	compatible = "memblock-v1";

  	n1 {
  		compatible = "reserve-mem-v1";
  		start = <0xc06b 0x4000000>;
  		size = <0x04 0x00>;
  	};

  	n2 {
  		compatible = "reserve-mem-v1";
  		start = <0xc067 0x4000000>;
  		size = <0x04 0x00>;
  	};
  };
