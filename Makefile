#
# Copyright (c) 2019. Quantum Corporation. All Rights Reserved.
# DXi, StorNext and Quantum are either a trademarks or registered
# trademarks of Quantum Corporation in the US and/or other countries.
#
# FICLONE/FICLONERANGE program makefile.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
# SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
# IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#

CFLAGS := -D_GNU_SOURCE=1 -std=c11

TARGET := cpr
TARGET_SRCS := cpr.c
TARGET_OBJS = $(TARGET_SRCS:.c=.o)

LIBTARGET := libcpr.a
LIBTARGET_SRCS := libcpr.c
LIBTARGET_OBJS = $(LIBTARGET_SRCS:.c=.o)

TEST_DIR   := tests
TEST_BUILD := $(TEST_DIR)/build
TEST_BIN   := $(TEST_BUILD)/test_libcpr

# The test binary wraps read()/write() to inject EINTR, short writes and I/O
# errors into the fallback copy loop. Set TEST_COVERAGE_FLAGS= to build the
# tests without gcov instrumentation (e.g. with clang).
TEST_COVERAGE_FLAGS ?= --coverage
TEST_LDFLAGS        := -Wl,--wrap=read,--wrap=write
COVERAGE_MIN        ?= 80
GCOV                ?= gcov

.PHONY: all
all: $(LIBTARGET) $(TARGET)

.PHONY: clean
clean:
	$(RM) $(TARGET_OBJS) $(LIBTARGET_OBJS)
	$(RM) $(TARGET) $(LIBTARGET)
	$(RM) -r $(TEST_BUILD)

.PHONY: test
test: $(TARGET) $(TEST_BIN)
	$(RM) $(TEST_BUILD)/*.gcda
	$(TEST_DIR)/run_tests.sh $(TEST_BIN) ./$(TARGET)

.PHONY: coverage
coverage: test
	GCOV=$(GCOV) $(TEST_DIR)/coverage.sh $(TEST_BUILD) $(COVERAGE_MIN)

$(TARGET): $(TARGET_OBJS) $(LIBTARGET)
	$(CC) -o $@ $^

$(LIBTARGET): $(LIBTARGET_OBJS)
	$(AR) cr $@ $^

$(TEST_BUILD):
	mkdir -p $@

$(TEST_BUILD)/libcpr.o: libcpr.c libcpr.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) -O0 $(TEST_COVERAGE_FLAGS) -c -o $@ libcpr.c

$(TEST_BUILD)/test_libcpr.o: $(TEST_DIR)/test_libcpr.c libcpr.h | $(TEST_BUILD)
	$(CC) $(CFLAGS) -I. -c -o $@ $(TEST_DIR)/test_libcpr.c

$(TEST_BIN): $(TEST_BUILD)/test_libcpr.o $(TEST_BUILD)/libcpr.o
	$(CC) -o $@ $^ $(TEST_COVERAGE_FLAGS) $(TEST_LDFLAGS)
