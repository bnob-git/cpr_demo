/**
 * @brief Fault-injection tests for the copy_file_range tier.
 *
 * Linked with -Wl,--wrap=copy_file_range so the copy_file_range(2) calls made
 * by libcpr go through __wrap_copy_file_range() below.
 *
 * Environment:
 * - CPR_TEST_DIR Scratch directory (required).
 */

#include "test_util.h"

#define FILE_LEN ((size_t)1024 * 1024 + 7)
#define PARTIAL  ((size_t)65536)

typedef enum
{
  FAULT_NONE,
  FAULT_PARTIAL_THEN_EXDEV, /* Copy PARTIAL bytes, then fail with EXDEV. */
  FAULT_EINTR_ONCE,         /* First call fails with EINTR. */
  FAULT_EIO,                /* Every call fails with EIO. */
  FAULT_ENOSYS,             /* Every call fails with ENOSYS. */
  FAULT_ZERO,               /* Every call reports EOF. */
} fault_t;

static fault_t g_fault = FAULT_NONE;
static int     g_calls = 0;

/**
 * Stand-in for the kernel's copy_file_range(2) so these tests do not depend on
 * kernel support. Copies at most one 64 KiB chunk per call.
 */

static ssize_t emulate_copy_file_range (int src_fd, off64_t *p_src_off,
                                        int dst_fd, off64_t *p_dst_off,
                                        size_t len)
{
  static uint8_t buf[65536];

  const ssize_t n = pread(src_fd, buf, len < sizeof(buf) ? len : sizeof(buf),
                          *p_src_off);

  if (n <= 0)
  {
    return n;
  }

  const ssize_t w = pwrite(dst_fd, buf, (size_t)n, *p_dst_off);

  if (w > 0)
  {
    *p_src_off += w;
    *p_dst_off += w;
  }

  return w;
}

ssize_t __wrap_copy_file_range (int src_fd, off64_t *p_src_off, int dst_fd,
                                off64_t *p_dst_off, size_t len,
                                unsigned int flags)
{
  const int call = g_calls++;

  switch (g_fault)
  {
    case FAULT_NONE:
      break;

    case FAULT_PARTIAL_THEN_EXDEV:
      if (call == 0)
      {
        len = len < PARTIAL ? len : PARTIAL;
        break;
      }
      errno = EXDEV;
      return -1;

    case FAULT_EINTR_ONCE:
      if (call == 0)
      {
        errno = EINTR;
        return -1;
      }
      break;

    case FAULT_EIO:
      errno = EIO;
      return -1;

    case FAULT_ENOSYS:
      errno = ENOSYS;
      return -1;

    case FAULT_ZERO:
      return 0;
  }

  (void)flags;

  return emulate_copy_file_range(src_fd, p_src_off, dst_fd, p_dst_off, len);
}

/*============================================================================*/

static char g_src[PATH_MAX];
static char g_dst[PATH_MAX];

static int run (const fault_t fault, const qtm_tier_t tiers,
                qtm_tier_t *p_used)
{
  g_fault = fault;
  g_calls = 0;
  *p_used = QTM_TIER_NONE;

  int src_fd = open_src(g_src);
  int dst_fd = open_dst(g_dst);
  int rc     = qtm_clone_file_ex(src_fd, dst_fd, tiers, RW_BLOCK_SIZE, p_used);

  close(src_fd);
  close(dst_fd);

  return rc;
}

/*============================================================================*/

int main (void)
{
  const char *dir = getenv("CPR_TEST_DIR");

  if (dir == NULL || dir[0] == '\0')
  {
    fprintf(stderr, "CPR_TEST_DIR must be set.\n");
    return EXIT_FAILURE;
  }

  join_path(g_src, dir, "fault_src");
  join_path(g_dst, dir, "fault_dst");
  CHECK(write_pattern_file(g_src, FILE_LEN, 11) == 0);

  /* Clone is left out so the result does not depend on reflink support. */
  const qtm_tier_t cfr_rw = QTM_TIER_CFR | QTM_TIER_RW;
  qtm_tier_t       used   = QTM_TIER_NONE;

  /* Sanity: the wrapper is really in the call path. */
  CHECK_RC(run(FAULT_NONE, cfr_rw, &used), 0);
  CHECK(used == QTM_TIER_CFR);
  CHECK(g_calls > 0);
  CHECK(files_equal(g_src, g_dst));

  /* copy_file_range stops part-way: read/write resumes and finishes. */
  CHECK_RC(run(FAULT_PARTIAL_THEN_EXDEV, cfr_rw, &used), 0);
  CHECK(used == (QTM_TIER_CFR | QTM_TIER_RW));
  CHECK(files_equal(g_src, g_dst));

  /* Same, for a range at non-zero offsets. */
  {
    g_fault = FAULT_PARTIAL_THEN_EXDEV;
    g_calls = 0;

    int src_fd = open_src(g_src);
    int dst_fd = open_dst(g_dst);

    CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, 100, 0, FILE_LEN - 100,
                                     cfr_rw, RW_BLOCK_SIZE, &used), 0);
    CHECK(used == (QTM_TIER_CFR | QTM_TIER_RW));

    close(src_fd);
    close(dst_fd);

    size_t   s_len = 0;
    size_t   d_len = 0;
    uint8_t *p_s   = read_whole_file(g_src, &s_len);
    uint8_t *p_d   = read_whole_file(g_dst, &d_len);

    CHECK(p_s != NULL && p_d != NULL && d_len == FILE_LEN - 100 &&
          memcmp(p_s + 100, p_d, d_len) == 0);

    free(p_s);
    free(p_d);
  }

  /* Without the read/write tier the copy_file_range error is returned. */
  CHECK_RC(run(FAULT_PARTIAL_THEN_EXDEV, QTM_TIER_CFR, &used), EXDEV);

  /* EINTR is retried. */
  CHECK_RC(run(FAULT_EINTR_ONCE, cfr_rw, &used), 0);
  CHECK(used == QTM_TIER_CFR);
  CHECK(files_equal(g_src, g_dst));

  /* "Not supported" errors fall back to read/write. */
  CHECK_RC(run(FAULT_ENOSYS, cfr_rw, &used), 0);
  CHECK(used == QTM_TIER_RW);
  CHECK(files_equal(g_src, g_dst));

  /* Real I/O errors are returned rather than retried with read/write. */
  CHECK_RC(run(FAULT_EIO, cfr_rw, &used), EIO);
  CHECK(g_calls == 1);

  /* A zero-length result for a file that has data falls back too. */
  CHECK_RC(run(FAULT_ZERO, cfr_rw, &used), 0);
  CHECK(used == QTM_TIER_RW);
  CHECK(files_equal(g_src, g_dst));

  return test_summary("test_cfr_faults");
}
