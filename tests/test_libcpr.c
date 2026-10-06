/**
 * Tests for libcpr: reflink, read/write fallback, range clones and error
 * paths.
 *
 * Environment:
 *   CPR_TEST_FALLBACK_DIR  Directory on a FS without reflink (tmpfs, ext4).
 *                          Defaults to a fresh directory under $TMPDIR.
 *   CPR_TEST_REFLINK_DIR   Directory on a FICLONE-capable FS (btrfs, xfs).
 *                          Reflink tests are skipped if unset.
 *   CPR_TEST_SUDO          Command prefix (e.g. "sudo -n") used to run
 *                          chattr when the caller lacks CAP_LINUX_IMMUTABLE.
 *   CPR_TEST_LABEL         Prefix for test names in the TAP output.
 *
 * Output is TAP. Link with -Wl,--wrap=read,--wrap=write so the fault
 * injection tests can force EINTR and short writes inside the copy loop.
 */

#include "libcpr.h"

#include <linux/fiemap.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*============================================================================*/
/* Fault injection via -Wl,--wrap. */

typedef struct _fault_t
{
  bool   armed;
  int    err;       /* errno to fail with; zero for a short write. */
  size_t short_len; /* Max bytes per call when err is zero. */
} fault_t;

static fault_t g_read_fault;
static fault_t g_write_fault;

ssize_t __real_read (int fd, void *buf, size_t count);
ssize_t __real_write (int fd, const void *buf, size_t count);

ssize_t __wrap_read (int fd, void *buf, size_t count)
{
  if (g_read_fault.armed)
  {
    g_read_fault.armed = false;
    errno = g_read_fault.err;
    return -1;
  }

  return __real_read(fd, buf, count);
}

ssize_t __wrap_write (int fd, const void *buf, size_t count)
{
  if (g_write_fault.armed)
  {
    g_write_fault.armed = false;

    if (g_write_fault.err != 0)
    {
      errno = g_write_fault.err;
      return -1;
    }

    if (count > g_write_fault.short_len)
    {
      count = g_write_fault.short_len;
    }
  }

  return __real_write(fd, buf, count);
}

#define MAX_SIZE(x_, y_) (((x_) >= (y_)) ? (x_) : (y_))

/*============================================================================*/
/* Minimal TAP harness. */

typedef struct _env_t
{
  const char *fallback_dir;
  const char *reflink_dir;
  const char *sudo;
  const char *label;
} env_t;

static int  g_test_num;
static int  g_failures;
static bool g_cur_failed;
static const char *g_skip_reason;

#define CHECK(cond_, ...)                                           \
  do {                                                              \
    if (!(cond_))                                                   \
    {                                                               \
      printf("#   %s:%d: CHECK(%s) failed: ", __FILE__, __LINE__,   \
             #cond_);                                               \
      printf(__VA_ARGS__);                                          \
      printf("\n");                                                 \
      g_cur_failed = true;                                          \
    }                                                               \
  } while (0)

#define CHECK_RC(got_, want_)                                       \
  CHECK((got_) == (want_), "got %d (%s), want %d (%s)",             \
        (got_), strerror(got_), (want_), strerror(want_))

#define SKIP(reason_)                                               \
  do {                                                              \
    g_skip_reason = (reason_);                                      \
    return;                                                         \
  } while (0)

typedef void (*test_fn_t)(const env_t *);

static void run_test (const env_t *env, const char *name, test_fn_t fn)
{
  g_cur_failed  = false;
  g_skip_reason = NULL;

  fn(env);

  g_test_num++;

  if (g_skip_reason != NULL)
  {
    printf("ok %d - %s%s # SKIP %s\n", g_test_num, env->label, name,
           g_skip_reason);
  }
  else if (g_cur_failed)
  {
    printf("not ok %d - %s%s\n", g_test_num, env->label, name);
    g_failures++;
  }
  else
  {
    printf("ok %d - %s%s\n", g_test_num, env->label, name);
  }

  fflush(stdout);
}

/*============================================================================*/
/* File helpers. */

static void fill_pattern (uint8_t *buf, size_t len, uint32_t seed)
{
  uint32_t x = seed | 1;

  for (size_t i = 0; i < len; i++)
  {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    buf[i] = (uint8_t)x;
  }
}

static char *path_in (const char *dir, const char *name)
{
  size_t len = strlen(dir) + strlen(name) + 2;
  char  *p   = malloc(len);

  if (p != NULL)
  {
    snprintf(p, len, "%s/%s", dir, name);
  }

  return p;
}

/** Create @p path with @p len pattern bytes (seeded by @p seed). */
static bool make_file (const char *path, size_t len, uint32_t seed)
{
  uint8_t *buf = malloc(len ? len : 1);
  bool     ok  = false;

  if (buf == NULL)
  {
    return false;
  }

  fill_pattern(buf, len, seed);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

  if (fd >= 0)
  {
    ok = (write(fd, buf, len) == (ssize_t)len);
    ok = (fsync(fd) == 0) && ok;
    ok = (close(fd) == 0) && ok;
  }

  free(buf);
  return ok;
}

/** Read all of @p path into a malloc()ed buffer. Returns NULL on failure. */
static uint8_t *slurp (const char *path, size_t *p_len)
{
  struct stat st;
  int         fd = open(path, O_RDONLY);

  if (fd < 0 || fstat(fd, &st) != 0)
  {
    if (fd >= 0)
    {
      close(fd);
    }
    return NULL;
  }

  uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
  size_t   got = 0;

  while (buf != NULL && got < (size_t)st.st_size)
  {
    ssize_t n = pread(fd, buf + got, st.st_size - got, got);

    if (n <= 0)
    {
      free(buf);
      buf = NULL;
      break;
    }

    got += n;
  }

  close(fd);
  *p_len = got;
  return buf;
}

static bool files_equal (const char *a, const char *b)
{
  size_t   alen = 0, blen = 0;
  uint8_t *abuf = slurp(a, &alen);
  uint8_t *bbuf = slurp(b, &blen);
  bool     eq   = abuf != NULL && bbuf != NULL && alen == blen &&
                  memcmp(abuf, bbuf, alen) == 0;

  free(abuf);
  free(bbuf);
  return eq;
}

static off_t file_size (const char *path)
{
  struct stat st;
  return (stat(path, &st) == 0) ? st.st_size : -1;
}

/** True if @p path has at least one extent flagged FIEMAP_EXTENT_SHARED. */
static bool has_shared_extent (const char *path)
{
  const unsigned int n_ext = 64;
  struct fiemap     *fm    =
    calloc(1, sizeof(*fm) + n_ext * sizeof(struct fiemap_extent));
  bool               shared = false;
  int                fd     = open(path, O_RDONLY);

  if (fm != NULL && fd >= 0)
  {
    fm->fm_start        = 0;
    fm->fm_length       = FIEMAP_MAX_OFFSET;
    fm->fm_flags        = FIEMAP_FLAG_SYNC;
    fm->fm_extent_count = n_ext;

    if (ioctl(fd, FS_IOC_FIEMAP, fm) == 0)
    {
      for (unsigned int i = 0; i < fm->fm_mapped_extents; i++)
      {
        shared = shared ||
                 (fm->fm_extents[i].fe_flags & FIEMAP_EXTENT_SHARED) != 0;
      }
    }
  }

  if (fd >= 0)
  {
    close(fd);
  }

  free(fm);
  return shared;
}

/**
 * Set or clear the immutable flag on @p path. Tries FS_IOC_SETFLAGS first and
 * falls back to "$CPR_TEST_SUDO chattr". Returns false if neither worked.
 */
static bool set_immutable (const env_t *env, const char *path, bool on)
{
  int fd = open(path, O_RDONLY);

  if (fd >= 0)
  {
    int  flags = 0;
    bool ok    = ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0;

    if (ok)
    {
      flags = on ? (flags | FS_IMMUTABLE_FL) : (flags & ~FS_IMMUTABLE_FL);
      ok    = ioctl(fd, FS_IOC_SETFLAGS, &flags) == 0;
    }

    close(fd);

    if (ok)
    {
      return true;
    }
  }

  if (env->sudo == NULL || env->sudo[0] == '\0')
  {
    return false;
  }

  char cmd[PATH_MAX + 128];
  snprintf(cmd, sizeof(cmd), "%s chattr %ci '%s' 2>/dev/null", env->sudo,
           on ? '+' : '-', path);

  return system(cmd) == 0;
}

/*============================================================================*/
/* Argument validation (no filesystem needed beyond open fds). */

static void t_invalid_args (const env_t *env)
{
  (void)env;

  CHECK_RC(qtm_clone_file(-1, 1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file(0, -1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file(0, 1, true, 0), EINVAL);

  CHECK_RC(qtm_clone_file_range(-1, 1, 0, 0, 1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file_range(0, -1, 0, 0, 1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file_range(0, 1, -1, 0, 1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file_range(0, 1, 0, -1, 1, false, 0), EINVAL);
  CHECK_RC(qtm_clone_file_range(0, 1, 0, 0, 1, true, 0), EINVAL);
}

/*============================================================================*/
/* Fallback (non-reflink FS) tests. */

typedef struct _pair_t
{
  char *src;
  char *dst;
  int   src_fd;
  int   dst_fd;
} pair_t;

/** Create src with @p src_len bytes and an empty dst in @p dir, opened. */
static bool pair_open (pair_t *p, const char *src_dir, const char *dst_dir,
                       size_t src_len, int dst_flags)
{
  p->src    = path_in(src_dir, "src");
  p->dst    = path_in(dst_dir, "dst");
  p->src_fd = -1;
  p->dst_fd = -1;

  if (p->src == NULL || p->dst == NULL || !make_file(p->src, src_len, 42))
  {
    return false;
  }

  unlink(p->dst);

  p->src_fd = open(p->src, O_RDONLY);
  p->dst_fd = open(p->dst, O_CREAT | dst_flags, 0644);

  return p->src_fd >= 0 && p->dst_fd >= 0;
}

static void pair_close (pair_t *p)
{
  if (p->src_fd >= 0)
  {
    close(p->src_fd);
  }

  if (p->dst_fd >= 0)
  {
    close(p->dst_fd);
  }

  if (p->src != NULL)
  {
    unlink(p->src);
  }

  if (p->dst != NULL)
  {
    unlink(p->dst);
  }

  free(p->src);
  free(p->dst);
}

static void t_fallback_disabled_fails (const env_t *env)
{
  pair_t p;

  if (pair_open(&p, env->fallback_dir, env->fallback_dir, 10000, O_WRONLY))
  {
    /* fallback_copy_block_size is ignored when fallback is off. */
    int rc = qtm_clone_file(p.src_fd, p.dst_fd, false, 0);
    CHECK(rc != 0, "FICLONE unexpectedly succeeded on fallback dir");
    CHECK(file_size(p.dst) == 0, "dst modified without fallback");

    rc = qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 4096, false, 0);
    CHECK(rc != 0, "FICLONERANGE unexpectedly succeeded on fallback dir");
    CHECK(file_size(p.dst) == 0, "dst modified without fallback");
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_fallback_whole_file_sizes (const env_t *env)
{
  static const size_t sizes[]  = { 0, 1, 4095, 4096, 4097, 65536 + 3,
                                   (1 << 20) + 13 };
  static const size_t blocks[] = { 1, 7, 512, 4096, 65536, 8 << 20 };

  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    for (size_t j = 0; j < sizeof(blocks) / sizeof(blocks[0]); j++)
    {
      /* Byte-at-a-time over a megabyte is slow and adds no coverage. */
      if (blocks[j] < 512 && sizes[i] > 65536 + 3)
      {
        continue;
      }

      pair_t p;

      if (!pair_open(&p, env->fallback_dir, env->fallback_dir, sizes[i],
                     O_WRONLY))
      {
        CHECK(false, "setup failed: %s", strerror(errno));
        pair_close(&p);
        return;
      }

      int rc = qtm_clone_file(p.src_fd, p.dst_fd, true, blocks[j]);
      CHECK_RC(rc, 0);
      CHECK(files_equal(p.src, p.dst),
            "dst differs from src (size %zu, block %zu)", sizes[i],
            blocks[j]);

      pair_close(&p);
    }
  }
}

/** Range copy must place src[src_off..] at dst[dst_off..] and leave the
 *  rest of dst untouched. */
static void check_range (const char *src, const char *dst, const uint8_t *orig,
                         size_t orig_len, off_t src_off, off_t dst_off,
                         size_t len)
{
  size_t   slen = 0, dlen = 0;
  uint8_t *s    = slurp(src, &slen);
  uint8_t *d    = slurp(dst, &dlen);

  CHECK(s != NULL && d != NULL, "could not read back files");

  if (s != NULL && d != NULL)
  {
    size_t want_len = MAX_SIZE(orig_len, (size_t)dst_off + len);

    CHECK(dlen == want_len, "dst length %zu, want %zu", dlen, want_len);
    CHECK(dlen >= (size_t)dst_off + len && slen >= (size_t)src_off + len &&
          memcmp(d + dst_off, s + src_off, len) == 0,
          "copied range differs");

    for (size_t i = 0; i < dlen && i < want_len; i++)
    {
      if (i >= (size_t)dst_off && i < (size_t)dst_off + len)
      {
        continue;
      }

      uint8_t want = (i < orig_len) ? orig[i] : 0;

      if (d[i] != want)
      {
        CHECK(false, "byte %zu outside range changed (0x%02x != 0x%02x)", i,
              d[i], want);
        break;
      }
    }
  }

  free(s);
  free(d);
}

/** Run a fallback range copy into a dst pre-filled with @p dst_len bytes. */
static void range_case (const env_t *env, const char *dir, size_t src_len,
                        size_t dst_len, off_t src_off, off_t dst_off,
                        size_t len, size_t want_len, bool fallback,
                        size_t block_size, int want_rc)
{
  pair_t   p;
  uint8_t *orig = malloc(dst_len ? dst_len : 1);

  (void)env;

  if (orig == NULL ||
      !pair_open(&p, dir, dir, src_len, O_WRONLY))
  {
    CHECK(false, "setup failed: %s", strerror(errno));
    free(orig);
    pair_close(&p);
    return;
  }

  fill_pattern(orig, dst_len, 7);
  CHECK(pwrite(p.dst_fd, orig, dst_len, 0) == (ssize_t)dst_len,
        "prefill dst failed");

  int rc = qtm_clone_file_range(p.src_fd, p.dst_fd, src_off, dst_off, len,
                                fallback, block_size);
  CHECK_RC(rc, want_rc);

  if (rc == 0)
  {
    check_range(p.src, p.dst, orig, dst_len, src_off, dst_off, want_len);
  }

  free(orig);
  pair_close(&p);
}

static void t_fallback_range_offsets (const env_t *env)
{
  /* Inside existing dst data, odd block size. */
  range_case(env, env->fallback_dir, 10000, 3000, 3, 5, 1000, 1000, true, 7,
             0);
  /* Extends dst. */
  range_case(env, env->fallback_dir, 10000, 3000, 4096, 2500, 5000, 5000,
             true, 4096, 0);
  /* Starts past dst EOF, leaving a zero-filled gap. */
  range_case(env, env->fallback_dir, 10000, 3000, 0, 5000, 4097, 4097, true,
             65536, 0);
}

static void t_fallback_range_to_eof (const env_t *env)
{
  /* length == 0 means "to source EOF". */
  range_case(env, env->fallback_dir, 10000, 0, 100, 10, 0, 10000 - 100, true,
             4096, 0);
  range_case(env, env->fallback_dir, 10000, 20000, 0, 0, 0, 10000, true, 3,
             0);
}

static void t_fallback_range_past_eof (const env_t *env)
{
  pair_t p;

  if (pair_open(&p, env->fallback_dir, env->fallback_dir, 10000, O_WRONLY))
  {
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 5000, 0, 6000, true,
                                  4096), ERANGE);
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 20000, 0, 1, true,
                                  4096), ERANGE);
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

/* Regression: seek_file() used to truncate lseek()'s off_t into an int. */
static void t_fallback_large_offsets (const env_t *env)
{
  const off_t  src_off = ((off_t)2 << 30) + 12345;
  const off_t  dst_off = ((off_t)3 << 30) + 7;
  const size_t len     = 8192;
  uint8_t      want[8192], got[8192];
  char        *src     = path_in(env->fallback_dir, "bigsrc");
  char        *dst     = path_in(env->fallback_dir, "bigdst");
  int          sfd     = -1, dfd = -1;

  fill_pattern(want, len, 99);

  if (src == NULL || dst == NULL)
  {
    CHECK(false, "setup failed");
    goto out;
  }

  sfd = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
  dfd = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);

  if (sfd < 0 || dfd < 0 ||
      pwrite(sfd, want, len, src_off) != (ssize_t)len)
  {
    if (errno == EFBIG)
    {
      g_skip_reason = "filesystem does not support files > 2GiB";
    }
    else
    {
      CHECK(false, "setup failed: %s", strerror(errno));
    }
    goto out;
  }

  CHECK_RC(qtm_clone_file_range(sfd, dfd, src_off, dst_off, len, true, 4096),
           0);
  CHECK(pread(dfd, got, len, dst_off) == (ssize_t)len &&
        memcmp(got, want, len) == 0, "data at 3GiB offset differs");
  CHECK(file_size(dst) == dst_off + (off_t)len, "dst size wrong");

out:
  if (sfd >= 0)
  {
    close(sfd);
  }

  if (dfd >= 0)
  {
    close(dfd);
  }

  if (src != NULL)
  {
    unlink(src);
  }

  if (dst != NULL)
  {
    unlink(dst);
  }

  free(src);
  free(dst);
}

static void t_fallback_bad_fds (const env_t *env)
{
  pair_t p;

  if (pair_open(&p, env->fallback_dir, env->fallback_dir, 10000, O_RDONLY))
  {
    /* dst opened read-only: write() fails. */
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, true, 4096), EBADF);
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 100, true, 4096),
             EBADF);

    /* src opened write-only: read() fails. */
    int wsrc = open(p.src, O_WRONLY);
    int wdst = open(p.dst, O_WRONLY);
    CHECK_RC(qtm_clone_file(wsrc, wdst, true, 4096), EBADF);
    CHECK_RC(qtm_clone_file_range(wsrc, wdst, 0, 0, 100, true, 4096), EBADF);
    close(wsrc);
    close(wdst);
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_fallback_src_is_directory (const env_t *env)
{
  char *dst  = path_in(env->fallback_dir, "dst");
  int   sfd  = open(env->fallback_dir, O_RDONLY | O_DIRECTORY);
  int   dfd  = (dst != NULL) ? open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                             : -1;

  CHECK(sfd >= 0 && dfd >= 0, "setup failed: %s", strerror(errno));

  if (sfd >= 0 && dfd >= 0)
  {
    CHECK_RC(qtm_clone_file(sfd, dfd, true, 4096), EISDIR);
  }

  if (sfd >= 0)
  {
    close(sfd);
  }

  if (dfd >= 0)
  {
    close(dfd);
  }

  if (dst != NULL)
  {
    unlink(dst);
  }

  free(dst);
}

static void t_fallback_pipes (const env_t *env)
{
  pair_t p;
  int    fds[2];

  if (pipe(fds) != 0 ||
      !pair_open(&p, env->fallback_dir, env->fallback_dir, 100, O_WRONLY))
  {
    CHECK(false, "setup failed: %s", strerror(errno));
    pair_close(&p);
    return;
  }

  /* Source seek fails. */
  CHECK_RC(qtm_clone_file(fds[0], p.dst_fd, true, 4096), ESPIPE);
  /* Source seek succeeds, destination seek fails. */
  CHECK_RC(qtm_clone_file(p.src_fd, fds[1], true, 4096), ESPIPE);
  CHECK_RC(qtm_clone_file_range(p.src_fd, fds[1], 0, 0, 10, true, 4096),
           ESPIPE);

  close(fds[0]);
  close(fds[1]);
  pair_close(&p);
}

static void t_fallback_enomem (const env_t *env)
{
  pair_t p;

  if (pair_open(&p, env->fallback_dir, env->fallback_dir, 100, O_WRONLY))
  {
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, true, SIZE_MAX), ENOMEM);
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 10, true,
                                  SIZE_MAX), ENOMEM);
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

/** Whole-file fallback copy with one injected fault. */
static void fault_case (const env_t *env, fault_t *fault, fault_t setting,
                        int want_rc)
{
  pair_t p;

  if (!pair_open(&p, env->fallback_dir, env->fallback_dir, 10000, O_WRONLY))
  {
    CHECK(false, "setup failed: %s", strerror(errno));
    pair_close(&p);
    return;
  }

  *fault = setting;
  int rc = qtm_clone_file(p.src_fd, p.dst_fd, true, 4096);
  bool fired = !fault->armed;
  fault->armed = false;

  CHECK(fired, "injected fault was never hit");
  CHECK_RC(rc, want_rc);

  if (want_rc == 0)
  {
    CHECK(files_equal(p.src, p.dst), "dst differs from src");
  }

  pair_close(&p);
}

static void t_fault_read_eintr (const env_t *env)
{
  fault_case(env, &g_read_fault,
             (fault_t){ .armed = true, .err = EINTR }, 0);
}

static void t_fault_write_eintr (const env_t *env)
{
  fault_case(env, &g_write_fault,
             (fault_t){ .armed = true, .err = EINTR }, 0);
}

static void t_fault_short_write (const env_t *env)
{
  fault_case(env, &g_write_fault,
             (fault_t){ .armed = true, .err = 0, .short_len = 100 }, 0);
}

static void t_fault_read_error (const env_t *env)
{
  fault_case(env, &g_read_fault,
             (fault_t){ .armed = true, .err = EIO }, EIO);
}

static void t_fault_write_error (const env_t *env)
{
  fault_case(env, &g_write_fault,
             (fault_t){ .armed = true, .err = ENOSPC }, ENOSPC);
}

/*============================================================================*/
/* Reflink (FICLONE-capable FS) tests. */

#define NEED_REFLINK(env_)                                            \
  do {                                                                \
    if ((env_)->reflink_dir == NULL)                                  \
    {                                                                 \
      SKIP("CPR_TEST_REFLINK_DIR not set");                           \
    }                                                                 \
  } while (0)

static void t_reflink_whole_file (const env_t *env)
{
  NEED_REFLINK(env);

  pair_t p;

  if (pair_open(&p, env->reflink_dir, env->reflink_dir, (1 << 20) + 13,
                O_WRONLY))
  {
    /* Block size is ignored when fallback is off. */
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, false, 0), 0);
    CHECK(files_equal(p.src, p.dst), "dst differs from src");
    CHECK(has_shared_extent(p.dst), "dst has no shared extents");
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_reflink_preferred_over_fallback (const env_t *env)
{
  NEED_REFLINK(env);

  pair_t p;

  if (pair_open(&p, env->reflink_dir, env->reflink_dir, 1 << 20, O_WRONLY))
  {
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, true, 4096), 0);
    CHECK(files_equal(p.src, p.dst), "dst differs from src");
    CHECK(has_shared_extent(p.dst),
          "dst has no shared extents (fallback used instead of reflink?)");
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_reflink_range_aligned (const env_t *env)
{
  NEED_REFLINK(env);

  range_case(env, env->reflink_dir, 1 << 20, 256 << 10, 4096, 8192, 65536,
             65536, false, 0, 0);
  /* length == 0 clones to source EOF, including an unaligned tail. */
  range_case(env, env->reflink_dir, (1 << 20) + 13, 0, 4096, 0, 0,
             (1 << 20) + 13 - 4096, false, 0, 0);

  pair_t p;

  if (pair_open(&p, env->reflink_dir, env->reflink_dir, 1 << 20, O_WRONLY))
  {
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 1 << 20, false,
                                  0), 0);
    CHECK(has_shared_extent(p.dst), "dst has no shared extents");
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_reflink_range_unaligned (const env_t *env)
{
  NEED_REFLINK(env);

  /* FICLONERANGE needs block-aligned offsets... */
  range_case(env, env->reflink_dir, 10000, 3000, 1, 5, 100, 100, false, 0,
             EINVAL);
  /* ...so fallback must kick in and still produce the right bytes. */
  range_case(env, env->reflink_dir, 10000, 3000, 1, 5, 100, 100, true, 7, 0);
}

static void t_cross_fs (const env_t *env)
{
  NEED_REFLINK(env);

  pair_t p;

  if (pair_open(&p, env->fallback_dir, env->reflink_dir, 100000, O_WRONLY))
  {
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, false, 0), EXDEV);
    CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 4096, false, 0),
             EXDEV);
    CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, true, 4096), 0);
    CHECK(files_equal(p.src, p.dst), "dst differs from src");
  }
  else
  {
    CHECK(false, "setup failed: %s", strerror(errno));
  }

  pair_close(&p);
}

static void t_immutable_dst (const env_t *env)
{
  NEED_REFLINK(env);

  pair_t p;

  if (!pair_open(&p, env->reflink_dir, env->reflink_dir, 65536, O_WRONLY))
  {
    CHECK(false, "setup failed: %s", strerror(errno));
    pair_close(&p);
    return;
  }

  if (!set_immutable(env, p.dst, true))
  {
    pair_close(&p);
    SKIP("cannot set immutable flag (need root or CPR_TEST_SUDO)");
  }

  CHECK_RC(qtm_clone_file(p.src_fd, p.dst_fd, false, 0), EPERM);
  CHECK_RC(qtm_clone_file_range(p.src_fd, p.dst_fd, 0, 0, 4096, false, 0),
           EPERM);
  CHECK(file_size(p.dst) == 0, "immutable dst was modified");

  /* The kernel checks immutability at open() for write(2), so a deep copy
   * through an fd opened before chattr +i is not rejected. Record what
   * happens rather than asserting on it.
   */
  int rc = qtm_clone_file(p.src_fd, p.dst_fd, true, 4096);
  printf("# note: fallback copy into immutable dst returned %d (%s)\n", rc,
         strerror(rc));

  CHECK(set_immutable(env, p.dst, false), "could not clear immutable flag");
  pair_close(&p);
}

/*============================================================================*/

int main (void)
{
  char        tmpl[PATH_MAX];
  bool        own_dir = false;
  env_t       env     =
  {
    .fallback_dir = getenv("CPR_TEST_FALLBACK_DIR"),
    .reflink_dir  = getenv("CPR_TEST_REFLINK_DIR"),
    .sudo         = getenv("CPR_TEST_SUDO"),
    .label        = getenv("CPR_TEST_LABEL"),
  };

  if (env.label == NULL)
  {
    env.label = "";
  }

  if (env.reflink_dir != NULL && env.reflink_dir[0] == '\0')
  {
    env.reflink_dir = NULL;
  }

  if (env.fallback_dir == NULL || env.fallback_dir[0] == '\0')
  {
    const char *tmp = getenv("TMPDIR");
    snprintf(tmpl, sizeof(tmpl), "%s/cpr-test.XXXXXX",
             (tmp != NULL && tmp[0] != '\0') ? tmp : "/tmp");

    if (mkdtemp(tmpl) == NULL)
    {
      perror("mkdtemp");
      return EXIT_FAILURE;
    }

    env.fallback_dir = tmpl;
    own_dir          = true;
  }

  printf("# fallback dir: %s\n", env.fallback_dir);
  printf("# reflink dir:  %s\n",
         env.reflink_dir != NULL ? env.reflink_dir : "(none)");

#define RUN(fn_) run_test(&env, #fn_, fn_)

  RUN(t_invalid_args);
  RUN(t_fallback_disabled_fails);
  RUN(t_fallback_whole_file_sizes);
  RUN(t_fallback_range_offsets);
  RUN(t_fallback_range_to_eof);
  RUN(t_fallback_range_past_eof);
  RUN(t_fallback_large_offsets);
  RUN(t_fallback_bad_fds);
  RUN(t_fallback_src_is_directory);
  RUN(t_fallback_pipes);
  RUN(t_fallback_enomem);
  RUN(t_fault_read_eintr);
  RUN(t_fault_write_eintr);
  RUN(t_fault_short_write);
  RUN(t_fault_read_error);
  RUN(t_fault_write_error);
  RUN(t_reflink_whole_file);
  RUN(t_reflink_preferred_over_fallback);
  RUN(t_reflink_range_aligned);
  RUN(t_reflink_range_unaligned);
  RUN(t_cross_fs);
  RUN(t_immutable_dst);

#undef RUN

  printf("1..%d\n", g_test_num);

  if (own_dir)
  {
    rmdir(tmpl);
  }

  return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
