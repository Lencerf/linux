.. SPDX-License-Identifier: GPL-2.0-or-later

===========================
File Descriptor Box (FDBox)
===========================

:Author: Pratyush Yadav

Introduction
============

The File Descriptor Box (FDBox) is a mechanism for userspace to name file
descriptors and give them over to the kernel to hold. They can later be
retrieved by passing in the same name.

The primary purpose of FDBox is to be used with :ref:`kho`. There are many kinds
anonymous file descriptors in the kernel like memfd, guest_memfd, iommufd, etc.
that would be useful to be preserved using KHO. To be able to do that, there
needs to be a mechanism to label FDs that allows userspace to set the label
before doing KHO and to use the label to map them back after KHO. FDBox achieves
that purpose by exposing a miscdevice which exposes ioctls to label and transfer
FDs between the kernel and userspace. FDBox is not intended to work with any
generic file descriptor. Support for each kind of FDs must be explicitly
enabled.

FDBox can be enabled by setting the ``CONFIG_FDBOX`` option to ``y``. While the
primary purpose of FDBox is to be used with KHO, it does not explicitly require
``CONFIG_KEXEC_HANDOVER``, since it can be used without KHO, simply as a way to
preserve or transfer FDs when userspace exits.

Concepts
========

Box
---

The box is a container for FDs. Boxes are identified by their name, which must
be unique. Userspace can put FDs in the box using the ``FDBOX_PUT_FD``
operation, and take them out of the box using the ``FDBOX_GET_FD`` operation.
Once all the required FDs are put into the box, it can be sealed to make it
ready for shipping. This can be done by the ``FDBOX_SEAL`` operation. The seal
operation notifies each FD in the box. If any of the FDs have a dependency on
another, this gives them an opportunity to ensure all dependencies are met, or
fail the seal if not. Once a box is sealed, no FDs can be added or removed from
the box until it is unsealed. Only sealed boxes are transported to a new kernel
via KHO. The box can be unsealed by the ``FDBOX_UNSEAL`` operation. This is the
opposite of seal. It also notifies each FD in the box to ensure all dependencies
are met. This can be useful in case some FDs fail to be restored after KHO.

Box FD
------

The Box FD is a FD that is currently in a box. It is identified by its name,
which must be unique in the box it belongs to. The Box FD is created when a FD
is put into a box by using the ``FDBOX_PUT_FD`` operation. This operation
removes the FD from the calling task. The FD can be restored by passing the
unique name to the ``FDBOX_GET_FD`` operation.

FDBox control device
--------------------

This is the ``/dev/fdbox/fdbox`` device. A box can be created using the
``FDBOX_CREATE_BOX`` operation on the device. A box can be removed using the
``FDBOX_DELETE_BOX`` operation.

UAPI
====

FDBOX_NAME_LEN
--------------

.. code-block:: c

    #define FDBOX_NAME_LEN			256

Maximum length of the name of a Box or Box FD.

Ioctls on /dev/fdbox/fdbox
--------------------------

FDBOX_CREATE_BOX
~~~~~~~~~~~~~~~~

.. code-block:: c

    #define FDBOX_CREATE_BOX	_IO(FDBOX_TYPE, FDBOX_BASE + 0)
    struct fdbox_create_box {
    	__u64 flags;
    	__u8 name[FDBOX_NAME_LEN];
    };

Create a box.

After this returns, the box is available at ``/dev/fdbox/<name>``.

``name``
    The name of the box to be created. Must be unique.

``flags``
    Flags to the operation. Currently, no flags are defined.

Returns:
    0 on success, -1 on error, with errno set.

FDBOX_DELETE_BOX
~~~~~~~~~~~~~~~~

.. code-block:: c

    #define FDBOX_DELETE_BOX	_IO(FDBOX_TYPE, FDBOX_BASE + 1)
    struct fdbox_delete_box {
    	__u64 flags;
    	__u8 name[FDBOX_NAME_LEN];
    };

Delete a box.

After this returns, the box is no longer available at ``/dev/fdbox/<name>``.

``name``
    The name of the box to be deleted.

``flags``
    Flags to the operation. Currently, no flags are defined.

Returns:
    0 on success, -1 on error, with errno set.

Ioctls on /dev/fdbox/<boxname>
------------------------------

These must be performed on the ``/dev/fdbox/<boxname>`` device.

FDBX_PUT_FD
~~~~~~~~~~~

.. code-block:: c

    #define FDBOX_PUT_FD	_IO(FDBOX_TYPE, FDBOX_BASE + 2)
    struct fdbox_put_fd {
    	__u64 flags;
    	__u32 fd;
    	__u32 pad;
    	__u8 name[FDBOX_NAME_LEN];
    };


Put FD into the box.

After this returns, ``fd`` is removed from the task and can no longer be used by
it.

``name``
    The name of the FD.

``fd``
    The file descriptor number to be

``flags``
    Flags to the operation. Currently, no flags are defined.

Returns:
    0 on success, -1 on error, with errno set.

FDBX_GET_FD
~~~~~~~~~~~

.. code-block:: c

    #define FDBOX_GET_FD	_IO(FDBOX_TYPE, FDBOX_BASE + 3)
    struct fdbox_get_fd {
    	__u64 flags;
    	__u8 name[FDBOX_NAME_LEN];
    };

Get an FD from the box.

After this returns, the FD identified by ``name`` is mapped into the task and is
available for use.

``name``
    The name of the FD to get.

``flags``
    Flags to the operation. Currently, no flags are defined.

Returns:
    FD number on success, -1 on error with errno set.

FDBOX_SEAL
~~~~~~~~~~

.. code-block:: c

    #define FDBOX_SEAL	_IO(FDBOX_TYPE, FDBOX_BASE + 4)

Seal the box.

Gives the kernel an opportunity to ensure all dependencies are met in the box.
After this returns, the box is sealed and FDs can no longer be added or removed
from it. A box must be sealed for it to be transported across KHO.

Returns:
    0 on success, -1 on error with errno set.

FDBOX_UNSEAL
~~~~~~~~~~~~

.. code-block:: c

    #define FDBOX_UNSEAL	_IO(FDBOX_TYPE, FDBOX_BASE + 5)

Unseal the box.

Gives the kernel an opportunity to ensure all dependencies are met in the box,
and in case of KHO, no FDs have been lost in transit.

Returns:
    0 on success, -1 on error with errno set.

Kernel functions and structures
===============================

.. kernel-doc:: include/linux/fdbox.h
