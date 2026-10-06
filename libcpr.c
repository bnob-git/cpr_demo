/**
 * Copyright (c) 2018-2019. Quantum Corporation. All Rights Reserved.
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
 * @section notes Notes
 *
 * Pretty-much all of the errors that could be returned from
 * FICLONE/FICLONERANGE indicate that reflink @e may not be supported.
 *
 * - EXDEV (src and dst on different FS) -> need to deep copy.
 * - ENOSYS (FICLONE ioctl missing) -> need to deep copy bytes.
 * - EBADF (src unreadable, dst unwritable or dst on non-reflink FS) ->
 *   may need to deep copy.
 * - EINVAL (fs doesn't support reflink, or perhaps the block alignment
 *   of the requested reflink is not supported) -> need to deep copy.
 * - EPERM (dst is immutable). Also returned by write() if we deep copy.
 * - EISDIR (src or dst is a directory and FS doesn't support directory
 *   reflinks) -> try deep copy.
 * - etc.
 *
 * For real failures (e.g. dst unwritable, dst immutable) a read/write
 * deep copy will return a real error that we can tell the caller.
 */

#include "libcpr.h"

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/*============================================================================*/

/**
 * Find the smaller of two objects that are the same type.
 *
 * @note This has the potential to double-evaluate its input parameters, so do
 *       not invoke it with parameters that have side-effects.
 */
#define MIN(x_, y_) (((x_) <= (y_)) ? (x_) : (y_))

/** Largest request passed to a single copy_file_range(2) call. */
#define CFR_CHUNK_MAX ((size_t)1 << 30)

/*============================================================================*/

/**
 * Clone a range from @p src_fd into @p dst_fd.
 *
 * @note This operation may fail for any number of reasons. Pretty-much all of
 *       them are able to indicate that the FICLONERANGE IOCTL is not
 *       available or not supported on the particular combination of
 *       parameters.
 *
 * @param[in] src_fd     Source file.
 * @param[in] dst_fd     Destination file.
 * @param[in] src_offset Offset to start clone from.
 * @param[in] dst_offset Offset to start clone to.
 * @param[in] length     Length of clone.
 * @return 0 for success, non-zero errno value on failure.
 */

static int clone_file_range_impl (const int    src_fd,
                                  const int    dst_fd,
                                  const off_t  src_offset,
                                  const off_t  dst_offset,
                                  const size_t length)

{
  struct file_clone_range clone_range =
  {
    .src_fd      = src_fd,
    .src_offset  = src_offset,
    .src_length  = length,
    .dest_offset = dst_offset
  };

  int rc = ioctl(dst_fd, FICLONERANGE, &clone_range);

  if (rc < 0)
  {
    rc = errno;
  }

  return rc;
}

/*============================================================================*/

/**
 * Clone an entire file from @p src_fd into @p dst_fd.
 *
 * @note This operation may fail for any number of reasons. Pretty-much all of
 *       them are able to indicate that the FICLONE IOCTL is not
 *       available or not supported on the particular combination of
 *       parameters.
 *
 * @param[in] src_fd Source file.
 * @param[in] dst_fd Destination file.
 * @return 0 for success, non-zero errno value on failure.
 */

static int clone_file_impl (const int src_fd, const int dst_fd)
{
  int rc = ioctl(dst_fd, FICLONE, src_fd);

  if (rc < 0)
  {
    rc = errno;
  }

  return rc;
}

/*============================================================================*/

/**
 * Seek a file descriptor, @p fd, to a desired @p offset.
 *
 * @param[in] fd     Source file.
 * @param[in] offset Offset from start.
 * @return Zero on success, some errno value on failure.
 */

static int seek_file (const int fd, const off_t offset)
{
  /* lseek() returns the current offset on success. Keep it in an off_t so
   * that offsets >= 2GiB are not truncated, and return zero on success.
   */
  return (lseek(fd, offset, SEEK_SET) == (off_t)-1) ? errno : 0;
}

/*============================================================================*/

/**
 * Write a block to @p fd, blocking until the whole amount is written or
 * an error prevents writing more.
 *
 * @param[in] fd      Destination file.
 * @param[in] p_block Data to write.
 * @param[in] length  Length of data in @p p_block to write.
 * @return Zero on success, some errno value on failure.
 */

static int write_block (const int      fd,
                        const uint8_t *p_block,
                        size_t         length)
{
  int rc = 0;

  while (length > 0)
  {
    ssize_t wrote_now = write(fd, p_block, length);

    if (wrote_now < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      rc = errno;
      break;
    }

    p_block += wrote_now;
    length  -= wrote_now;
  }

  return rc;
}

/*============================================================================*/

/**
 * Copy @p length bytes from @p src_fd into @p dst_fd.
 *
 * @param[in] src_fd     Source file.
 * @param[in] dst_fd     Destination file.
 * @param[in] src_offset Offset to start copy from.
 * @param[in] dst_offset Offset to start copy to.
 * @param[in] length     Length of segment to copy. Zero to copy to source EOF.
 * @param[in] block_size Block size to use when copying.
 * @return Zero on success, some error value on failure.
 */

static int deep_copy_file_range_impl (const int    src_fd,
                                      const int    dst_fd,
                                      const off_t  src_offset,
                                      const off_t  dst_offset,
                                      const size_t length,
                                      const size_t block_size)
{
  int rc = seek_file(src_fd, src_offset);

  if (rc == 0)
  {
    rc = seek_file(dst_fd, dst_offset);
  }

  if (rc != 0)
  {
    return rc;
  }

  void *p_block = malloc(block_size * sizeof(uint8_t));

  if (p_block == NULL)
  {
    return ENOMEM;
  }

  size_t remain = (length != 0) ? length : block_size;

  while (remain > 0)
  {
    const size_t  read_max = MIN(block_size, remain);
    const ssize_t read_now = read(src_fd, p_block, read_max);

    if (read_now < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      rc = errno;
      break;
    }
    else if (read_now == 0 && length == 0)
    {
      /* EOF was reached and we were copying to EOF. Terminate loop. */
      break;
    }
    else if (read_now == 0 && remain != 0)
    {
      /* EOF was reached before the requested copy length. */
      rc = ERANGE;
      break;
    }

    rc = write_block(dst_fd, p_block, read_now);

    if (rc != 0)
    {
      break;
    }

    /* Only update the remaining length if not copying to EOF. */
    if (length != 0)
    {
      remain -= read_now;
    }
  }

  /* Cleanup. */
  free(p_block);

  return rc;
}

/*============================================================================*/

/**
 * Call copy_file_range(2), via the raw system call if the C library is too old
 * to provide a wrapper (glibc < 2.27).
 */

static ssize_t sys_copy_file_range (int     src_fd,
                                    loff_t *p_src_offset,
                                    int     dst_fd,
                                    loff_t *p_dst_offset,
                                    size_t  length)
{
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 27))
  return copy_file_range(src_fd, p_src_offset, dst_fd, p_dst_offset, length,
                         0);
#elif defined(SYS_copy_file_range)
  return syscall(SYS_copy_file_range, src_fd, p_src_offset, dst_fd,
                 p_dst_offset, length, 0U);
#else
  (void)src_fd;
  (void)p_src_offset;
  (void)dst_fd;
  (void)p_dst_offset;
  (void)length;
  errno = ENOSYS;
  return -1;
#endif
}

/*============================================================================*/

/**
 * Decide whether a copy_file_range(2) error means the call is not possible for
 * this pair of files (so a read/write copy should be tried) rather than a real
 * I/O failure that read/write would also hit.
 */

static bool cfr_error_is_unsupported (const int rc)
{
  switch (rc)
  {
    case EXDEV:      /* Different file systems (Linux >= 5.19 for any pair). */
    case EINVAL:     /* Unsupported file type, overlapping same-file range. */
    case ENOSYS:     /* Kernel < 4.5 or syscall filtered. */
    case EOPNOTSUPP: /* File system does not support it. */
#if ENOTSUP != EOPNOTSUPP
    case ENOTSUP:
#endif
    case EBADF:      /* e.g. dst opened with O_APPEND. */
    case EPERM:      /* e.g. syscall blocked by a seccomp profile. */
    case ETXTBSY:
      return true;

    default:
      return false;
  }
}

/*============================================================================*/

/**
 * Copy a range from @p src_fd into @p dst_fd with copy_file_range(2).
 *
 * Explicit offsets are passed so the file offsets of @p src_fd and @p dst_fd
 * are not modified.
 *
 * @param[in]  src_fd     Source file.
 * @param[in]  dst_fd     Destination file.
 * @param[in]  src_offset Offset to start copy from.
 * @param[in]  dst_offset Offset to start copy to.
 * @param[in]  length     Length of segment to copy. Zero to copy to source EOF.
 * @param[out] p_copied   Number of bytes copied, also on failure.
 * @return Zero on success, some errno value on failure. ERANGE if the source
 *         ended before @p length bytes were copied.
 */

static int cfr_copy_range_impl (const int    src_fd,
                                const int    dst_fd,
                                const off_t  src_offset,
                                const off_t  dst_offset,
                                const size_t length,
                                size_t      *p_copied)
{
  loff_t src_pos = src_offset;
  loff_t dst_pos = dst_offset;
  size_t copied  = 0;
  int    rc      = 0;

  while (length == 0 || copied < length)
  {
    const size_t  want   = (length != 0) ? MIN(length - copied, CFR_CHUNK_MAX)
                                         : CFR_CHUNK_MAX;
    const ssize_t copied_now =
      sys_copy_file_range(src_fd, &src_pos, dst_fd, &dst_pos, want);

    if (copied_now < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      rc = errno;
      break;
    }
    else if (copied_now == 0)
    {
      /* Zero means EOF, but some pseudo file systems also return zero for
       * files that do have data. Only trust it if fstat() agrees.
       */
      struct stat src_stat;

      if (fstat(src_fd, &src_stat) == 0 && src_stat.st_size > src_pos)
      {
        rc = EOPNOTSUPP;
      }
      else if (length != 0)
      {
        /* EOF was reached before the requested copy length. */
        rc = ERANGE;
      }

      break;
    }

    copied += copied_now;
  }

  *p_copied = copied;

  return rc;
}

/*============================================================================*/

/**
 * After a whole-file copy, shrink a longer pre-existing regular destination to
 * the source size. None of the tiers do that by themselves (FICLONE of an
 * empty source is a no-op).
 */

static int truncate_dst_to_src (const int src_fd, const int dst_fd)
{
  struct stat src_stat;
  struct stat dst_stat;

  if (fstat(src_fd, &src_stat) != 0 || fstat(dst_fd, &dst_stat) != 0)
  {
    return errno;
  }

  if (S_ISREG(src_stat.st_mode) && S_ISREG(dst_stat.st_mode) &&
      dst_stat.st_size > src_stat.st_size &&
      ftruncate(dst_fd, src_stat.st_size) != 0)
  {
    return errno;
  }

  return 0;
}

/*============================================================================*/

/**
 * Copy from @p src_fd into @p dst_fd, trying each tier in @p tiers in order.
 * See qtm_clone_file_ex() for the semantics.
 */

static int copy_with_tiers (const int    src_fd,
                            const int    dst_fd,
                            const bool   whole_file,
                            const off_t  src_offset,
                            const off_t  dst_offset,
                            const size_t length,
                            const qtm_tier_t tiers,
                            const size_t rw_block_size,
                            qtm_tier_t  *p_tier_used)
{
  if (src_fd < 0 || dst_fd < 0 || src_offset < 0 || dst_offset < 0 ||
      tiers == QTM_TIER_NONE || (tiers & ~QTM_TIER_ALL) != 0 ||
      ((tiers & QTM_TIER_RW) && rw_block_size == 0))
  {
    return EINVAL;
  }

  qtm_tier_t used     = QTM_TIER_NONE;
  off_t      src_pos  = src_offset;
  off_t      dst_pos  = dst_offset;
  size_t     remain   = length;
  int        rc       = EINVAL;

  if (tiers & QTM_TIER_CLONE)
  {
    rc = whole_file
      ? clone_file_impl(src_fd, dst_fd)
      : clone_file_range_impl(src_fd, dst_fd, src_offset, dst_offset, length);

    if (rc == 0)
    {
      used = QTM_TIER_CLONE;
    }
  }

  if (used == QTM_TIER_NONE && (tiers & QTM_TIER_CFR))
  {
    size_t copied = 0;

    rc = cfr_copy_range_impl(src_fd, dst_fd, src_pos, dst_pos, remain,
                             &copied);

    if (rc == 0)
    {
      used = QTM_TIER_CFR;
    }
    else if (cfr_error_is_unsupported(rc) && (tiers & QTM_TIER_RW))
    {
      /* Let the read/write tier pick up from wherever this stopped. */
      src_pos += copied;
      dst_pos += copied;

      if (length != 0)
      {
        remain -= copied;
      }

      if (copied > 0)
      {
        used = QTM_TIER_CFR;
      }
    }
    else
    {
      return rc;
    }
  }

  if (rc != 0 && (tiers & QTM_TIER_RW))
  {
    rc = deep_copy_file_range_impl(src_fd, dst_fd, src_pos, dst_pos, remain,
                                   rw_block_size);

    if (rc == 0)
    {
      used |= QTM_TIER_RW;
    }
  }

  if (rc == 0 && whole_file)
  {
    rc = truncate_dst_to_src(src_fd, dst_fd);
  }

  if (rc == 0 && p_tier_used != NULL)
  {
    *p_tier_used = used;
  }

  return rc;
}

/*============================================================================*/

int qtm_clone_file_ex (const int         src_fd,
                       const int         dst_fd,
                       const qtm_tier_t  tiers,
                       const size_t      rw_block_size,
                       qtm_tier_t       *p_tier_used)
{
  return copy_with_tiers(src_fd, dst_fd, true, 0, 0, 0, tiers, rw_block_size,
                         p_tier_used);
}

/*============================================================================*/

int qtm_clone_file_range_ex (const int         src_fd,
                             const int         dst_fd,
                             const off_t       src_offset,
                             const off_t       dst_offset,
                             const size_t      length,
                             const qtm_tier_t  tiers,
                             const size_t      rw_block_size,
                             qtm_tier_t       *p_tier_used)
{
  return copy_with_tiers(src_fd, dst_fd, false, src_offset, dst_offset, length,
                         tiers, rw_block_size, p_tier_used);
}

/*============================================================================*/

int qtm_clone_file (const int    src_fd,
                    const int    dst_fd,
                    const bool   fallback_copy,
                    const size_t fallback_copy_block_size)
{
  return qtm_clone_file_ex(src_fd, dst_fd,
                           fallback_copy ? QTM_TIER_ALL : QTM_TIER_CLONE,
                           fallback_copy_block_size, NULL);
}

/*============================================================================*/

int qtm_clone_file_range (const int    src_fd,
                          const int    dst_fd,
                          const off_t  src_offset,
                          const off_t  dst_offset,
                          const size_t length,
                          const bool   fallback_copy,
                          const size_t fallback_copy_block_size)
{
  return qtm_clone_file_range_ex(src_fd, dst_fd, src_offset, dst_offset,
                                 length,
                                 fallback_copy ? QTM_TIER_ALL : QTM_TIER_CLONE,
                                 fallback_copy_block_size, NULL);
}

/*============================================================================*/
