.. SPDX-License-Identifier: GPL-2.0-or-later

=======
KHO FDT
=======

KHO uses the flattened device tree (FDT) container format and libfdt
library to create and parse the data that is passed between the
kernels. The properties in KHO FDT are stored in native format and can
include any data KHO users need to preserve. Parsing of FDT subnodes is
responsibility of KHO users, except for properties defined by KHO itself.

KHO properties
==============

Compatible
----------

The ``compatible`` property determines compatibility between the kernel
that created the KHO FDT and the kernel that attempts to load it.
If the kernel that loads the KHO FDT is not compatible with it, the entire
KHO process will be bypassed.

mem
---

KHO defines a ``mem`` property that can be inside any subnode in the
FDT. This property represents an array of physical memory ranges that the
new kernel must preserve on boot. Every element of the array is represented
with::

    struct kho_mem {
            __u64 addr;
            __u64 len;
    };

Examples
========

The following example demonstrates KHO FDT that preserves two memory
regions create with ``reserve_mem`` kernel command line parameter::

  /dts-v1/;

  / {
  	compatible = "kho-v1";

  	reserve-mem {
  		compatible = "reserve-mem-v1";

  		region1 {
  			compatible = "reserve-mem-map-v1";
  			mem = <0xc07a 0x4000000 0x01 0x00>;
  		};

		region2 {
			compatible = "reserve-mem-map-v1";
			mem = <0xc07b 0x4000000 0x8000 0x00>;
		};

  	};
  };
