/**
 * Copyright (c) 2019. Quantum Corporation. All Rights Reserved.
 * DXi, StorNext and Quantum are either a trademarks or registered
 * trademarks of Quantum Corporation in the US and/or other countries.
 *
 * @brief FICLONE/FICLONERANGE test library.
 *
 * @note This file is standard C11. It does not use any Quantum-specific
 *       code or libraries so it can be called on BTRFS/ext4/XFS file-systems
 *       on non-DXi platforms. Compile with @e -D_GNU_SOURCE=1.
 *
 * @section license License
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * @section description Description
 *
 * This package exports two methods, #qtm_clone_file() + #qtm_clone_file_range(),
 * which are intended to exercise the Linux FICLONE or FICLONERANGE ioctls
 * repsectively.
 *
 * In the case reflink is not supported, both methods provide the ability to
 * automatically fall back on a copy_file_range(2) copy and then on a
 * read()/write() deep copy. Falling back will prevent the caller from finding
 * out what caused the reflink operation to fail. The caller should decide
 * whether they are interested in why reflink failed before blindly requesing
 * the auto-fallback.
 *
 * #qtm_clone_file_ex() and #qtm_clone_file_range_ex() expose the individual
 * copy tiers (see #qtm_tier_t) so that callers can select which tiers may be
 * attempted and find out which tier(s) performed the copy.
 *
 * Despite the @c qtm_ prefix on the exported method names, this code is not
 * specific to Quantum file systems and will work on any file system that
 * provides the ability to reflink on Linux. With fallback enabled the copy
 * will work on any two file handles that can be seeked, read and written.
 */

#include <sys/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================*/

/**
 * Bitmask of copy tiers. Tiers are always attempted in the order listed here;
 * a tier that is not in the mask is skipped.
 *
 * - #QTM_TIER_CLONE: FICLONE / FICLONERANGE ioctl (reflink, copy-on-write).
 * - #QTM_TIER_CFR: copy_file_range(2). The kernel copies the data and may
 *   offload it to the storage or file server. Only works when both files are
 *   on the same file system instance (EXDEV otherwise since Linux 5.19).
 * - #QTM_TIER_RW: userspace read(2)/write(2) loop.
 *
 * @{
 */

typedef unsigned int qtm_tier_t;

#define QTM_TIER_NONE  0x00u
#define QTM_TIER_CLONE 0x01u
#define QTM_TIER_CFR   0x02u
#define QTM_TIER_RW    0x04u
#define QTM_TIER_ALL   (QTM_TIER_CLONE | QTM_TIER_CFR | QTM_TIER_RW)

/** @} */

/*============================================================================*/

/**
 * Attempt to clone the entire file @p src_fd into @p dst_fd, overwriting its
 * contents.
 *
 * This function will invoke the FICLONE ioctl. Equivalent to
 * #qtm_clone_file_ex() with @c QTM_TIER_ALL if @p fallback_copy is set, or
 * @c QTM_TIER_CLONE if it is clear.
 *
 * @param[in] src_fd
 *   Source file.
 * @param[in] dst_fd
 *   Destination file.
 * @param[in] fallback_copy
 *   If set, fall back to copy_file_range(2) and then a deep read()/write()
 *   copy if the FICLONE call fails.
 * @param[in] fallback_copy_block_size
 *   Block size to use if @p fallback_copy is set. Must be larger than zero.
 *   Ignored if @p fallback_copy is clear.
 * @return
 *   Zero on success. Some non-zero errno value on failure.
 *
 *   If @p fallback_copy is clear then one of the errno values from
 *   ioctl-ficlonerange(2) may be returned including, but not limited to, the
 *   following:
 *
 *   - @c EBADF
 *   - @c EINVAL
 *   - @c EISDIR
 *   - @c EOPNOTSUPP
 *   - @c EPERM
 *   - @c ETXTBUSY
 *   - @c EXDEV
 *
 *   If  @p fallback_copy is set then one of the errno values from lseek(),
 *   copy_file_range(2), read(2) or write(2) may be returned including, but
 *   not limited to, the following:
 *
 *   - @c EAGAIN or @c EWOULDBLOCK (for non-blocking file descriptors)
 *   - @c EBADF
 *   - @c EDESTADDRREQ
 *   - @c EDQUOT
 *   - @c EFAULT
 *   - @c EFBIG
 *   - @c EINVAL (also if @p fallback_copy_block_size was zero)
 *   - @c EIO
 *   - @c ENOSPC
 *   - @c EOVERFLOW
 *   - @c ESPIPE (one of @p src_fd or @p dst_fd is a socket, pipe or FIFO)
 *   - @c EISDIR
 *   - @c ENOMEM (could not allocate memory for @p fallback_copy_block_size)
 *   - @c ENXIO
 */

int qtm_clone_file (const int    src_fd,
                    const int    dst_fd,
                    const bool   fallback_copy,
                    const size_t fallback_copy_block_size);

/*============================================================================*/

/**
 * Attempt to clone a range from the file @p src_fd into @p dst_fd,
 * overwriting any existing data at that range.
 *
 * This function will invoke the FICLONERANGE ioctl. Equivalent to
 * #qtm_clone_file_range_ex() with @c QTM_TIER_ALL if @p fallback_copy is set,
 * or @c QTM_TIER_CLONE if it is clear.
 *
 * @param[in] src_fd
 *   Source file.
 * @param[in] dst_fd
 *   Destination file.
 * @param[in] src_offset
 *   Offset into @p src_fd to begin the clone.
 * @param[in] dst_offset
 *   Offset into @p dst_fd to stitch the cloned data.
 * @param[in] length
 *   Number of bytes to clone.
 * @param[in] fallback_copy
 *   If set, fall back to copy_file_range(2) and then a deep read()/write()
 *   copy if the FICLONERANGE call fails.
 * @param[in] fallback_copy_block_size
 *   Block size to use if @p fallback_copy is set. Must be larger than zero.
 *   Ignored if @p fallback_copy is clear.
 * @return
 *   Zero on success. Some non-zero errno value on failure.
 *
 *   If @p fallback_copy is clear then one of the errno values from
 *   ioctl-ficlonerange(2) may be returned including, but not limited to, the
 *   following:
 *
 *   - @c EBADF
 *   - @c EINVAL
 *   - @c EISDIR
 *   - @c EOPNOTSUPP
 *   - @c EPERM
 *   - @c ETXTBUSY
 *   - @c EXDEV
 *
 *   If  @p fallback_copy is set then one of the errno values from lseek(),
 *   copy_file_range(2), read(2) or write(2) may be returned including, but
 *   not limited to, the following:
 *
 *   - @c EAGAIN or @c EWOULDBLOCK (for non-blocking file descriptors)
 *   - @c EBADF
 *   - @c EDESTADDRREQ
 *   - @c EDQUOT
 *   - @c EFAULT
 *   - @c EFBIG
 *   - @c EINVAL (also if @p fallback_copy_block_size was zero)
 *   - @c EIO
 *   - @c ENOSPC
 *   - @c EOVERFLOW
 *   - @c ESPIPE (one of @p src_fd or @p dst_fd is a socket, pipe or FIFO)
 *   - @c EISDIR
 *   - @c ENOMEM (could not allocate memory for @p fallback_copy_block_size)
 *   - @c ENXIO
 */

int qtm_clone_file_range (const int    src_fd,
                          const int    dst_fd,
                          const off_t  src_offset,
                          const off_t  dst_offset,
                          const size_t length,
                          const bool   fallback_copy,
                          const size_t fallback_copy_block_size);

/*============================================================================*/

/**
 * Copy the entire file @p src_fd into @p dst_fd using the first tier in
 * @p tiers that succeeds.
 *
 * A failure of the clone tier always moves on to the next enabled tier. A
 * copy_file_range(2) failure moves on to the read/write tier only for errors
 * that mean "not possible here" (EXDEV, EINVAL, ENOSYS, EOPNOTSUPP, EBADF,
 * EPERM, ETXTBSY); other errors (e.g. ENOSPC, EIO) are returned as-is. If
 * copy_file_range(2) stops part-way, the read/write tier resumes from where it
 * stopped.
 *
 * The clone and copy_file_range(2) tiers do not move the file offsets of
 * @p src_fd or @p dst_fd; the read/write tier does.
 *
 * @param[in] src_fd
 *   Source file.
 * @param[in] dst_fd
 *   Destination file.
 * @param[in] tiers
 *   Non-empty mask of #QTM_TIER_CLONE, #QTM_TIER_CFR and #QTM_TIER_RW.
 * @param[in] rw_block_size
 *   Block size for the read/write tier. Must be larger than zero if
 *   @p tiers contains #QTM_TIER_RW, otherwise ignored.
 * @param[out] p_tier_used
 *   Optional (may be NULL). On success, set to the mask of tiers that copied
 *   data: a single tier, or @c QTM_TIER_CFR|QTM_TIER_RW if copy_file_range(2)
 *   stopped part-way and read/write finished the copy.
 * @return
 *   Zero on success. Some non-zero errno value on failure: the error from the
 *   last tier attempted, or EINVAL if @p tiers is empty or contains unknown
 *   bits. See #qtm_clone_file() for the possible values.
 */

int qtm_clone_file_ex (const int         src_fd,
                       const int         dst_fd,
                       const qtm_tier_t  tiers,
                       const size_t      rw_block_size,
                       qtm_tier_t       *p_tier_used);

/*============================================================================*/

/**
 * Copy a range from the file @p src_fd into @p dst_fd using the first tier in
 * @p tiers that succeeds. Tier semantics are as for #qtm_clone_file_ex().
 *
 * @param[in] src_fd
 *   Source file.
 * @param[in] dst_fd
 *   Destination file.
 * @param[in] src_offset
 *   Offset into @p src_fd to begin the copy.
 * @param[in] dst_offset
 *   Offset into @p dst_fd to stitch the copied data.
 * @param[in] length
 *   Number of bytes to copy. Zero to copy to the end of @p src_fd. ERANGE is
 *   returned by the copy_file_range(2) and read/write tiers if @p src_fd ends
 *   before @p length bytes were copied.
 * @param[in] tiers
 *   Non-empty mask of #QTM_TIER_CLONE, #QTM_TIER_CFR and #QTM_TIER_RW.
 * @param[in] rw_block_size
 *   Block size for the read/write tier. Must be larger than zero if
 *   @p tiers contains #QTM_TIER_RW, otherwise ignored.
 * @param[out] p_tier_used
 *   Optional (may be NULL). See #qtm_clone_file_ex().
 * @return
 *   Zero on success. Some non-zero errno value on failure. See
 *   #qtm_clone_file_range() for the possible values.
 */

int qtm_clone_file_range_ex (const int         src_fd,
                             const int         dst_fd,
                             const off_t       src_offset,
                             const off_t       dst_offset,
                             const size_t      length,
                             const qtm_tier_t  tiers,
                             const size_t      rw_block_size,
                             qtm_tier_t       *p_tier_used);

/*============================================================================*/

#ifdef __cplusplus
}
#endif

