SUMMARY
=======

FICLONE/FICLONERANGE test library and driver.

DESCRIPTION
===========

This repository contains the code for libcpr (that wraps FICLONE and
FICLONERANGE) and the driver program cpr which can exercise the functions
provided by libcpr.

The functions in libcpr are optionally able to fall back onto a
copy_file_range(2) copy and then a deep read(2)/write(2) copy if the
FICLONE/FICLONERANGE ioctls fail. This allows using libcpr generally without
the caller needing to be particularly concerned about the filesystem(s) which
the source and destination files reside. If it is possible to read/write copy
the source into the destination then it will be done.

COPY TIERS
==========

Copies are attempted in this order, skipping tiers that the caller disabled
(qtm_clone_file_ex() / qtm_clone_file_range_ex() take a QTM_TIER_* mask; the
cpr driver takes '-T clone,cfr,rw'):

1. clone (QTM_TIER_CLONE): FICLONE/FICLONERANGE reflink. Needs a
   reflink-capable filesystem (btrfs, xfs with reflink=1, ...).
2. cfr (QTM_TIER_CFR): copy_file_range(2). The kernel copies the data and
   may offload it (e.g. NFS server-side copy). Works within a single
   filesystem on ext4, xfs without reflink, tmpfs, etc. Since Linux 5.19 it
   fails with EXDEV between different filesystems.
3. rw (QTM_TIER_RW): read(2)/write(2) loop. Works for any seekable files.

'cpr -v' reports the tier(s) used. 'make test' runs the unit tests;
'make test-loopback' also runs them on loopback xfs/ext4 mounts so the clone
tier is covered (needs root or passwordless sudo). bench/bench_tiers.sh
compares the tiers on loopback ext4 and xfs.

REQUIREMENTS
============

Building this software requires a C11-compliant compiler and a Linux kernel
with the FICLONE and FICLONERANGE ioctls defined.

The copy_file_range(2) tier needs Linux 4.5 or newer; on older kernels it
fails with ENOSYS and the read/write tier is used. Copies between different
filesystems only work in the cfr tier on Linux 5.3 - 5.18; elsewhere they
fail with EXDEV and fall through to the read/write tier.

It has been tested with GCC 7.3.1 from the RedHat devtoolset-7 on CentOS
7.6.1810. The kernel in this CentOS release contains these ioctls. A 4.5 or
newer kernel will contain them by default.

BUILDING
========

Simply type 'make' in the current directory on a system which meets the above
requrements and the program should build.

If the C11 compiler is not the first in your path, or not in your path, then
set the CC variable to point at it. e.g. 'make CC=/path/to/c11'.

COPYRIGHT
=========

Copyright (c) 2019. Quantum Corporation. All Rights Reserved.
DXi, StorNext and Quantum are either a trademarks or registered
trademarks of Quantum Corporation in the US and/or other countries.

LICENSE
=======

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

