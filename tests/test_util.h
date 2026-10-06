/**
 * @brief Shared helpers for the libcpr tests.
 */

#include "libcpr.h"

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

#define RW_BLOCK_SIZE 8192

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(cond_)                                                     \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond_))                                                        \
    {                                                                    \
      g_failures++;                                                      \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
              #cond_);                                                   \
    }                                                                    \
  } while (0)

#define CHECK_RC(rc_, expected_)                                         \
  do {                                                                   \
    const int got_ = (rc_);                                              \
    g_checks++;                                                          \
    if (got_ != (expected_))                                             \
    {                                                                    \
      g_failures++;                                                      \
      fprintf(stderr, "%s:%d: expected %s (%s), got %d (%s)\n",          \
              __FILE__, __LINE__, #expected_, strerror(expected_),       \
              got_, strerror(got_));                                     \
    }                                                                    \
  } while (0)

static inline int test_summary (const char *name)
{
  printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static inline void join_path (char *out, const char *dir, const char *name)
{
  snprintf(out, PATH_MAX, "%s/%s", dir, name);
}

static inline void fill_pattern (uint8_t *p, const size_t len, uint32_t seed)
{
  for (size_t i = 0; i < len; i++)
  {
    seed = seed * 1103515245u + 12345u;
    p[i] = (uint8_t)(seed >> 16);
  }
}

/** Create @p path holding @p len pattern bytes generated from @p seed. */

static inline int write_pattern_file (const char *path, const size_t len,
                                      const uint32_t seed)
{
  uint8_t *p   = malloc(len ? len : 1);
  int      fd  = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int      rc  = (p == NULL || fd < 0) ? -1 : 0;

  if (rc == 0)
  {
    fill_pattern(p, len, seed);
    rc = (write(fd, p, len) == (ssize_t)len) ? 0 : -1;
  }

  if (fd >= 0)
  {
    close(fd);
  }

  free(p);

  return rc;
}

/** Read the whole of @p path into a malloc()ed buffer. NULL on failure. */

static inline uint8_t *read_whole_file (const char *path, size_t *p_len)
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

  uint8_t *p = malloc(st.st_size ? (size_t)st.st_size : 1);

  if (p != NULL && read(fd, p, st.st_size) != (ssize_t)st.st_size)
  {
    free(p);
    p = NULL;
  }

  close(fd);
  *p_len = (size_t)st.st_size;

  return p;
}

static inline bool files_equal (const char *a, const char *b)
{
  size_t   a_len = 0;
  size_t   b_len = 0;
  uint8_t *p_a   = read_whole_file(a, &a_len);
  uint8_t *p_b   = read_whole_file(b, &b_len);
  bool     same  = p_a != NULL && p_b != NULL && a_len == b_len &&
                   memcmp(p_a, p_b, a_len) == 0;

  free(p_a);
  free(p_b);

  return same;
}

static inline int open_src (const char *path)
{
  return open(path, O_RDONLY);
}

static inline int open_dst (const char *path)
{
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
