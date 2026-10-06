/**
 * @brief Tests for the libcpr copy tiers (clone, copy_file_range, read/write).
 *
 * Environment:
 * - CPR_TEST_DIR         Scratch directory (required).
 * - CPR_TEST_XDEV_DIR    Optional scratch directory on a different file
 *                        system to CPR_TEST_DIR.
 * - CPR_TEST_REFLINK_DIR Optional scratch directory on a reflink-capable file
 *                        system (btrfs, xfs -m reflink=1).
 */

#include "test_util.h"

#define FILE_LEN ((size_t)3 * 1024 * 1024 + 123)

/*============================================================================*/

/** Copy the whole of @p src into a fresh @p dst using @p tiers. */

static int copy_whole (const char *src, const char *dst,
                       const qtm_tier_t tiers, qtm_tier_t *p_used)
{
  int src_fd = open_src(src);
  int dst_fd = open_dst(dst);

  *p_used = QTM_TIER_NONE;

  int rc = qtm_clone_file_ex(src_fd, dst_fd, tiers, RW_BLOCK_SIZE, p_used);

  close(src_fd);
  close(dst_fd);

  return rc;
}

/*============================================================================*/

static void test_validation (const char *dir)
{
  char src[PATH_MAX];
  char dst[PATH_MAX];

  join_path(src, dir, "val_src");
  join_path(dst, dir, "val_dst");
  CHECK(write_pattern_file(src, 4096, 1) == 0);

  int        src_fd = open_src(src);
  int        dst_fd = open_dst(dst);
  qtm_tier_t used   = QTM_TIER_NONE;

  CHECK_RC(qtm_clone_file_ex(src_fd, dst_fd, QTM_TIER_NONE, RW_BLOCK_SIZE,
                             &used), EINVAL);
  CHECK_RC(qtm_clone_file_ex(src_fd, dst_fd, 0x08u, RW_BLOCK_SIZE, &used),
           EINVAL);
  CHECK_RC(qtm_clone_file_ex(src_fd, dst_fd, QTM_TIER_RW, 0, &used), EINVAL);
  CHECK_RC(qtm_clone_file_ex(-1, dst_fd, QTM_TIER_ALL, RW_BLOCK_SIZE, &used),
           EINVAL);
  CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, -1, 0, 0, QTM_TIER_ALL,
                                   RW_BLOCK_SIZE, &used), EINVAL);
  CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, 0, -1, 0, QTM_TIER_ALL,
                                   RW_BLOCK_SIZE, &used), EINVAL);

  /* Legacy API keeps its contract. */
  CHECK_RC(qtm_clone_file(src_fd, dst_fd, true, 0), EINVAL);
  CHECK_RC(qtm_clone_file_range(src_fd, dst_fd, 0, 0, 0, true, 0), EINVAL);

  /* Block size is ignored when the read/write tier is not enabled; a NULL
   * p_tier_used is allowed.
   */
  int rc = qtm_clone_file_ex(src_fd, dst_fd, QTM_TIER_CFR, 0, NULL);
  CHECK(rc == 0 || rc == EXDEV || rc == EOPNOTSUPP || rc == ENOSYS);

  close(src_fd);
  close(dst_fd);
}

/*============================================================================*/

/**
 * Whole-file copies with each tier and tier combination on one file system.
 *
 * @param[in] reflink_expected The file system must support FICLONE.
 */

static void test_whole_file (const char *dir, const bool reflink_expected)
{
  char       src[PATH_MAX];
  char       dst[PATH_MAX];
  qtm_tier_t used = QTM_TIER_NONE;

  join_path(src, dir, "whole_src");
  join_path(dst, dir, "whole_dst");
  CHECK(write_pattern_file(src, FILE_LEN, 2) == 0);

  /* Tier 1: clone only. */
  const int  clone_rc = copy_whole(src, dst, QTM_TIER_CLONE, &used);
  const bool clone_ok = clone_rc == 0;

  if (clone_ok)
  {
    CHECK(used == QTM_TIER_CLONE);
    CHECK(files_equal(src, dst));
  }

  if (reflink_expected)
  {
    CHECK_RC(clone_rc, 0);
  }

  printf("  %s: clone tier %s\n", dir,
         clone_ok ? "supported" : strerror(clone_rc));

  /* Tier 2: copy_file_range only. Same-FS copy_file_range works on Linux >=
   * 5.3 for all local file systems.
   */
  CHECK_RC(copy_whole(src, dst, QTM_TIER_CFR, &used), 0);
  CHECK(used == QTM_TIER_CFR);
  CHECK(files_equal(src, dst));

  /* Tier 3: read/write only. */
  CHECK_RC(copy_whole(src, dst, QTM_TIER_RW, &used), 0);
  CHECK(used == QTM_TIER_RW);
  CHECK(files_equal(src, dst));

  /* Full chain: clone if it works, otherwise copy_file_range. */
  CHECK_RC(copy_whole(src, dst, QTM_TIER_ALL, &used), 0);
  CHECK(used == (clone_ok ? QTM_TIER_CLONE : QTM_TIER_CFR));
  CHECK(files_equal(src, dst));

  /* copy_file_range disabled: skip straight to read/write. */
  CHECK_RC(copy_whole(src, dst, QTM_TIER_CLONE | QTM_TIER_RW, &used), 0);
  CHECK(used == (clone_ok ? QTM_TIER_CLONE : QTM_TIER_RW));
  CHECK(files_equal(src, dst));

  /* Clone disabled. */
  CHECK_RC(copy_whole(src, dst, QTM_TIER_CFR | QTM_TIER_RW, &used), 0);
  CHECK(used == QTM_TIER_CFR);
  CHECK(files_equal(src, dst));

  /* Legacy API: fallback runs the full chain, no fallback is clone only. */
  int src_fd = open_src(src);
  int dst_fd = open_dst(dst);

  CHECK_RC(qtm_clone_file(src_fd, dst_fd, true, RW_BLOCK_SIZE), 0);
  CHECK(files_equal(src, dst));
  CHECK_RC(qtm_clone_file(src_fd, dst_fd, false, 0), clone_rc);

  /* The clone and copy_file_range tiers leave the file offsets alone. */
  CHECK(lseek(src_fd, 0, SEEK_CUR) == 0);
  CHECK(lseek(dst_fd, 0, SEEK_CUR) == 0);

  close(src_fd);
  close(dst_fd);

  /* Empty source. */
  CHECK(write_pattern_file(src, 0, 3) == 0);

  const qtm_tier_t singles[] = { QTM_TIER_CFR, QTM_TIER_RW, QTM_TIER_ALL };

  for (size_t i = 0; i < sizeof(singles) / sizeof(singles[0]); i++)
  {
    CHECK(write_pattern_file(dst, 100, 4) == 0);
    CHECK_RC(copy_whole(src, dst, singles[i], &used), 0);
    CHECK(files_equal(src, dst));
  }
}

/*============================================================================*/

/** Range copies with the copy_file_range and read/write tiers. */

static void test_ranges (const char *dir)
{
  char src[PATH_MAX];
  char dst[PATH_MAX];

  join_path(src, dir, "range_src");
  join_path(dst, dir, "range_dst");
  CHECK(write_pattern_file(src, FILE_LEN, 5) == 0);

  const qtm_tier_t tiers[] = { QTM_TIER_CFR, QTM_TIER_RW,
                               QTM_TIER_CFR | QTM_TIER_RW };

  for (size_t i = 0; i < sizeof(tiers) / sizeof(tiers[0]); i++)
  {
    const off_t  src_off = 1000;
    const off_t  dst_off = 5000;
    const size_t len     = 70001;

    CHECK(write_pattern_file(dst, 200000, 6) == 0);

    int        src_fd = open_src(src);
    int        dst_fd = open(dst, O_WRONLY);
    qtm_tier_t used   = QTM_TIER_NONE;

    CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, src_off, dst_off, len,
                                     tiers[i], RW_BLOCK_SIZE, &used), 0);
    CHECK(used == (tiers[i] & QTM_TIER_CFR ? QTM_TIER_CFR : QTM_TIER_RW));

    size_t   s_len = 0;
    size_t   d_len = 0;
    uint8_t *p_s   = read_whole_file(src, &s_len);
    uint8_t *p_d   = read_whole_file(dst, &d_len);
    uint8_t *p_e   = malloc(200000);

    CHECK(p_s != NULL && p_d != NULL && p_e != NULL);

    if (p_s != NULL && p_d != NULL && p_e != NULL)
    {
      fill_pattern(p_e, 200000, 6);
      memcpy(p_e + dst_off, p_s + src_off, len);
      CHECK(d_len == 200000);
      CHECK(memcmp(p_d, p_e, 200000) == 0);
    }

    free(p_s);
    free(p_d);
    free(p_e);

    /* Length zero copies to the end of the source. */
    CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, 4096, 0, 0, tiers[i],
                                     RW_BLOCK_SIZE, &used), 0);
    CHECK(used == (tiers[i] & QTM_TIER_CFR ? QTM_TIER_CFR : QTM_TIER_RW));

    struct stat st;
    CHECK(fstat(dst_fd, &st) == 0 &&
          st.st_size == (off_t)(FILE_LEN - 4096));

    /* Source ends before the requested length. */
    CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, FILE_LEN - 10, 0, 100,
                                     tiers[i], RW_BLOCK_SIZE, &used), ERANGE);

    close(src_fd);
    close(dst_fd);
  }
}

/*============================================================================*/

/** Copies from @p dir to a different file system @p xdev_dir. */

static void test_cross_fs (const char *dir, const char *xdev_dir)
{
  char       src[PATH_MAX];
  char       dst[PATH_MAX];
  qtm_tier_t used = QTM_TIER_NONE;

  join_path(src, dir, "xdev_src");
  join_path(dst, xdev_dir, "xdev_dst");
  CHECK(write_pattern_file(src, FILE_LEN, 7) == 0);

  CHECK_RC(copy_whole(src, dst, QTM_TIER_CLONE, &used), EXDEV);

  /* Linux >= 5.19 refuses cross-file-system copy_file_range for local file
   * systems; 5.3 - 5.18 allowed it.
   */
  const int cfr_rc = copy_whole(src, dst, QTM_TIER_CFR, &used);

  CHECK(cfr_rc == 0 || cfr_rc == EXDEV);
  printf("  cross-fs: cfr tier %s\n", cfr_rc ? strerror(cfr_rc) : "supported");

  CHECK_RC(copy_whole(src, dst, QTM_TIER_ALL, &used), 0);
  CHECK(used == (cfr_rc == 0 ? QTM_TIER_CFR : QTM_TIER_RW));
  CHECK(files_equal(src, dst));

  CHECK_RC(copy_whole(src, dst, QTM_TIER_CLONE | QTM_TIER_CFR, &used),
           cfr_rc);
}

/*============================================================================*/

/** FICLONERANGE on a reflink-capable file system. */

static void test_reflink_range (const char *dir)
{
  char src[PATH_MAX];
  char dst[PATH_MAX];

  join_path(src, dir, "rl_src");
  join_path(dst, dir, "rl_dst");
  CHECK(write_pattern_file(src, 1024 * 1024, 8) == 0);
  CHECK(write_pattern_file(dst, 1024 * 1024, 9) == 0);

  int        src_fd = open_src(src);
  int        dst_fd = open(dst, O_WRONLY);
  qtm_tier_t used   = QTM_TIER_NONE;

  /* Block-aligned so FICLONERANGE can do it. */
  CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, 65536, 131072, 262144,
                                   QTM_TIER_ALL, RW_BLOCK_SIZE, &used), 0);
  CHECK(used == QTM_TIER_CLONE);

  /* Unaligned: FICLONERANGE fails with EINVAL, copy_file_range takes over. */
  CHECK_RC(qtm_clone_file_range_ex(src_fd, dst_fd, 1, 3, 1000, QTM_TIER_ALL,
                                   RW_BLOCK_SIZE, &used), 0);
  CHECK(used == QTM_TIER_CFR);

  close(src_fd);
  close(dst_fd);

  size_t   s_len = 0;
  size_t   d_len = 0;
  uint8_t *p_s   = read_whole_file(src, &s_len);
  uint8_t *p_d   = read_whole_file(dst, &d_len);
  uint8_t *p_e   = malloc(1024 * 1024);

  CHECK(p_s != NULL && p_d != NULL && p_e != NULL);

  if (p_s != NULL && p_d != NULL && p_e != NULL)
  {
    fill_pattern(p_e, 1024 * 1024, 9);
    memcpy(p_e + 131072, p_s + 65536, 262144);
    memcpy(p_e + 3, p_s + 1, 1000);
    CHECK(d_len == 1024 * 1024);
    CHECK(memcmp(p_d, p_e, 1024 * 1024) == 0);
  }

  free(p_s);
  free(p_d);
  free(p_e);
}

/*============================================================================*/

int main (void)
{
  const char *dir         = getenv("CPR_TEST_DIR");
  const char *xdev_dir    = getenv("CPR_TEST_XDEV_DIR");
  const char *reflink_dir = getenv("CPR_TEST_REFLINK_DIR");

  if (dir == NULL || dir[0] == '\0')
  {
    fprintf(stderr, "CPR_TEST_DIR must be set.\n");
    return EXIT_FAILURE;
  }

  test_validation(dir);
  test_whole_file(dir, false);
  test_ranges(dir);

  if (xdev_dir != NULL && xdev_dir[0] != '\0')
  {
    test_cross_fs(dir, xdev_dir);
  }
  else
  {
    printf("  SKIP cross-fs tests (CPR_TEST_XDEV_DIR not set)\n");
  }

  if (reflink_dir != NULL && reflink_dir[0] != '\0')
  {
    test_whole_file(reflink_dir, true);
    test_reflink_range(reflink_dir);
  }
  else
  {
    printf("  SKIP reflink tests (CPR_TEST_REFLINK_DIR not set)\n");
  }

  return test_summary("test_tiers");
}
