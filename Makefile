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

VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_PATCH := 0
VERSION := $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

PREFIX ?= /usr/local
EXEC_PREFIX ?= $(PREFIX)
BINDIR ?= $(EXEC_PREFIX)/bin
LIBDIR ?= $(EXEC_PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

INSTALL ?= install
LN_S ?= ln -sf

# User-overridable; the flags the build depends on live in CPR_* below.
CFLAGS ?= -Wall -Wextra

ifeq ($(DEBUG),1)
CPR_OPTFLAGS := -O0 -g3
else
CPR_OPTFLAGS := -O2 -DNDEBUG
endif

CPR_CPPFLAGS := -D_GNU_SOURCE=1
CPR_CFLAGS := -std=c11 -MMD -MP $(CPR_OPTFLAGS)

TARGET := cpr
TARGET_SRCS := cpr.c
TARGET_OBJS = $(TARGET_SRCS:.c=.o)

LIBNAME := libcpr
LIBHEADER := libcpr.h
LIBTARGET_SRCS := libcpr.c
LIBTARGET_OBJS = $(LIBTARGET_SRCS:.c=.o)

STATICLIB := $(LIBNAME).a
SHAREDLIB := $(LIBNAME).so
SONAME := $(SHAREDLIB).$(VERSION_MAJOR)
SHAREDLIB_REAL := $(SHAREDLIB).$(VERSION)
PCFILE := $(LIBNAME).pc

DEPS = $(TARGET_OBJS:.o=.d) $(LIBTARGET_OBJS:.o=.d)

.PHONY: all clean install uninstall FORCE

all: $(STATICLIB) $(SHAREDLIB) $(TARGET) $(PCFILE)

$(LIBTARGET_OBJS): CPR_CFLAGS += -fPIC

%.o: %.c
	$(CC) $(CPR_CPPFLAGS) $(CPPFLAGS) $(CPR_CFLAGS) $(CFLAGS) -c -o $@ $<

$(TARGET): $(TARGET_OBJS) $(STATICLIB)
	$(CC) $(CPR_OPTFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(STATICLIB): $(LIBTARGET_OBJS)
	$(RM) $@
	$(AR) rcs $@ $^

$(SHAREDLIB_REAL): $(LIBTARGET_OBJS)
	$(CC) $(CPR_OPTFLAGS) $(CFLAGS) $(LDFLAGS) -shared \
		-Wl,-soname,$(SONAME) -o $@ $^ $(LDLIBS)

$(SONAME): $(SHAREDLIB_REAL)
	$(LN_S) $< $@

$(SHAREDLIB): $(SONAME)
	$(LN_S) $< $@

# Escape \ & | and ' for a single-quoted sed replacement.
sed_repl = $(subst ','\'',$(subst |,\|,$(subst &,\&,$(subst \,\\,$(1)))))

# Regenerated on every run, but only rewritten when PREFIX/LIBDIR/etc. change.
$(PCFILE): $(PCFILE).in FORCE
	@sed -e 's|@PREFIX@|$(call sed_repl,$(PREFIX))|g' \
	     -e 's|@EXEC_PREFIX@|$(call sed_repl,$(EXEC_PREFIX))|g' \
	     -e 's|@LIBDIR@|$(call sed_repl,$(LIBDIR))|g' \
	     -e 's|@INCLUDEDIR@|$(call sed_repl,$(INCLUDEDIR))|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     $< > $@.tmp
	@if cmp -s $@.tmp $@; then $(RM) $@.tmp; \
	 else mv -f $@.tmp $@; echo "generated $@"; fi

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(LIBDIR)" \
		"$(DESTDIR)$(INCLUDEDIR)" "$(DESTDIR)$(PKGCONFIGDIR)"
	$(INSTALL) -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(INSTALL) -m 0644 $(STATICLIB) "$(DESTDIR)$(LIBDIR)/$(STATICLIB)"
	$(INSTALL) -m 0755 $(SHAREDLIB_REAL) "$(DESTDIR)$(LIBDIR)/$(SHAREDLIB_REAL)"
	$(LN_S) $(SHAREDLIB_REAL) "$(DESTDIR)$(LIBDIR)/$(SONAME)"
	$(LN_S) $(SONAME) "$(DESTDIR)$(LIBDIR)/$(SHAREDLIB)"
	$(INSTALL) -m 0644 $(LIBHEADER) "$(DESTDIR)$(INCLUDEDIR)/$(LIBHEADER)"
	$(INSTALL) -m 0644 $(PCFILE) "$(DESTDIR)$(PKGCONFIGDIR)/$(PCFILE)"

uninstall:
	$(RM) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	$(RM) "$(DESTDIR)$(LIBDIR)/$(STATICLIB)"
	$(RM) "$(DESTDIR)$(LIBDIR)/$(SHAREDLIB)"
	$(RM) "$(DESTDIR)$(LIBDIR)/$(SONAME)"
	$(RM) "$(DESTDIR)$(LIBDIR)/$(SHAREDLIB_REAL)"
	$(RM) "$(DESTDIR)$(INCLUDEDIR)/$(LIBHEADER)"
	$(RM) "$(DESTDIR)$(PKGCONFIGDIR)/$(PCFILE)"

clean:
	$(RM) $(TARGET_OBJS) $(LIBTARGET_OBJS) $(DEPS)
	$(RM) $(TARGET) $(STATICLIB) $(SHAREDLIB) $(SHAREDLIB).*
	$(RM) $(PCFILE) $(PCFILE).tmp

-include $(DEPS)
