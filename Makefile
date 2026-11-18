# SPDX-License-Identifier: MIT
# babysit - build, test, and package.
#
# Needs GNU make (gmake on the BSDs). clang/LLVM is the supported toolchain: the sanitizer and coverage targets
# use LLVM's runtimes and tools (llvm-profdata, llvm-cov).

CC       = clang
CFLAGS  ?= -O2 -g -Wall -Wextra -pedantic -Werror
PREFIX  ?= /usr/local
BINDIR  ?= bin
MANDIR  ?= $(PREFIX)/share/man
DOCDIR  ?= $(PREFIX)/share/doc/babysit
# systemd user units (Linux only): $(PREFIX)/lib/systemd/user suits /usr and
# /usr/local; for PREFIX=$$HOME/.local pass UNITDIR=$$HOME/.config/systemd/user.
# An empty UNITDIR installs no unit, which is the default off Linux.
OS      := $(shell uname -s)
ifeq ($(OS),Linux)
UNITDIR ?= $(PREFIX)/lib/systemd/user
else
UNITDIR ?=
endif
# Where bash looks for completions (the bash-completion package's
# directory). An empty COMPDIR installs none.
COMPDIR ?= $(PREFIX)/share/bash-completion/completions
# Where install-claude-skill and install-copilot-skill put the agent skill:
# that agent's personal skills directory. For one project, pass e.g.
# CLAUDE_SKILLDIR=/path/to/project/.claude/skills.
CLAUDE_SKILLDIR  ?= $(HOME)/.claude/skills
COPILOT_SKILLDIR ?= $(HOME)/.copilot/skills

VERSION := $(shell sed -n 's/^\#define BABYSIT_VERSION "\(.*\)"/\1/p' src/babysit.h)
ARCH    := $(shell uname -m)
BUILD    = build

# The sources are in src/. babysit is built from babysit.c, which holds
# main(), and the babysit_*.c files beside it; babysit_internal.h is what
# they share. babysitd is one file, in src/babysitd/ together with its
# service unit and the sample configuration file. babysit.h is shared by
# both programs.
BABYSIT_SRCS = src/babysit.c src/babysit_cmdline.c src/babysit_time.c \
               src/babysit_start.c src/babysit_sitter.c \
               src/babysit_output.c src/babysit_control.c \
               src/babysit_instance.c src/babysit_inspect.c \
               src/babysit_watch.c
BABYSIT_HDRS = src/babysit.h src/babysit_internal.h

PROGS    = $(BINDIR)/babysit $(BINDIR)/babysitd
TESTBINS = $(BINDIR)/lockprobe $(BINDIR)/faultinject.so
MANPAGES = docs/babysit.1 docs/babysitd.8 docs/babysit.conf.5
DISTFILES = Makefile LICENSE README.md precommit.sh .clang-format \
            .clang-tidy src \
            docs tests packaging skills completions \
            debian .gitlab-ci.yml

all: $(PROGS)

$(BINDIR)/babysit: $(BABYSIT_SRCS) $(BABYSIT_HDRS)
	@mkdir -p $(BINDIR)
	$(CC) -std=c99 $(CFLAGS) -o $@ $(BABYSIT_SRCS) $(LDFLAGS)

$(BINDIR)/babysitd: src/babysitd/babysitd.c src/babysit.h
	@mkdir -p $(BINDIR)
	$(CC) -std=c99 $(CFLAGS) -o $@ src/babysitd/babysitd.c $(LDFLAGS)

$(BINDIR)/lockprobe: tests/lockprobe.c src/babysit.h
	@mkdir -p $(BINDIR)
	$(CC) -std=c99 $(CFLAGS) -o $@ tests/lockprobe.c $(LDFLAGS)

# Preloaded by the fault tests to make one library call fail. Built with
# flags of its own: it must not carry a sanitizer or coverage runtime.
$(BINDIR)/faultinject.so: tests/faultinject.c
	@mkdir -p $(BINDIR)
	$(CC) -std=c99 -O1 -g -Wall -Wextra -Werror -fPIC -shared -o $@ \
	    tests/faultinject.c -ldl

$(BINDIR)/perftool: tests/perftool.c src/babysit.h
	@mkdir -p $(BINDIR)
	$(CC) -std=c99 $(CFLAGS) -o $@ tests/perftool.c $(LDFLAGS)

# ---- tests ---------------------------------------------------------------

test check: all $(TESTBINS)
	bash tests/run.sh --bindir $(BINDIR)

# What babysit costs under a light, a medium and a heavy load, set against
# earlier runs. Timings are kept in an SQLite database outside the tree;
# see tests/perf.sh --help.
perf: all $(BINDIR)/perftool
	bash tests/perf.sh --bindir $(BINDIR)

# Sanitizer builds. A sanitizer report inside the detached babysitter would
# otherwise go to /dev/null, so reports are written to files and any file
# fails the run.
SAN_asan  = address
SAN_ubsan = undefined
SAN_tsan  = thread
SAN_CFLAGS = -O1 -g -fno-omit-frame-pointer -fno-sanitize-recover=all \
             -Wall -Wextra -pedantic -Werror

check-asan check-ubsan check-tsan: check-%:
	rm -rf $(BUILD)/$*
	mkdir -p $(BUILD)/$*/reports
	$(MAKE) BINDIR=$(BUILD)/$* \
	    CFLAGS="$(SAN_CFLAGS) -fsanitize=$(SAN_$*)" all $(BUILD)/$*/lockprobe \
	    $(BUILD)/$*/faultinject.so
	ASAN_OPTIONS=log_path=$(CURDIR)/$(BUILD)/$*/reports/asan \
	UBSAN_OPTIONS=log_path=$(CURDIR)/$(BUILD)/$*/reports/ubsan:print_stacktrace=1 \
	TSAN_OPTIONS=log_path=$(CURDIR)/$(BUILD)/$*/reports/tsan \
	    bash tests/run.sh --bindir $(BUILD)/$*
	@if ls $(BUILD)/$*/reports/* >/dev/null 2>&1; then \
	    echo "sanitizer reports:"; cat $(BUILD)/$*/reports/*; exit 1; \
	else echo "$*: no sanitizer reports"; fi

# Coverage (LLVM source-based). Every process merges into the same raw
# profile, including the detached babysitter (see COVERAGE_FLUSH).
COVERAGE_MIN ?= 0
COV = $(BUILD)/coverage
COV_OBJECTS = $(COV)/babysit -object $(COV)/babysitd
COV_SOURCES = $(BABYSIT_SRCS) src/babysitd/babysitd.c src/babysit.h

coverage:
	rm -rf $(COV)
	mkdir -p $(COV)/raw
	$(MAKE) BINDIR=$(COV) CFLAGS="-O0 -g -Wall -Wextra -pedantic -Werror \
	    -fprofile-instr-generate \
	    -fcoverage-mapping -DBABYSIT_COVERAGE" all $(COV)/lockprobe \
	    $(COV)/faultinject.so
	LLVM_PROFILE_FILE=$(CURDIR)/$(COV)/raw/babysit-%m.profraw \
	    bash tests/run.sh --bindir $(COV)
	llvm-profdata merge -sparse $(COV)/raw/*.profraw -o $(COV)/babysit.profdata
	llvm-cov report $(COV_OBJECTS) -instr-profile=$(COV)/babysit.profdata \
	    $(COV_SOURCES) | tee $(COV)/report.txt
	llvm-cov show $(COV_OBJECTS) -instr-profile=$(COV)/babysit.profdata \
	    -format=html -output-dir=$(COV)/html $(COV_SOURCES) >/dev/null
	llvm-cov export $(COV_OBJECTS) -instr-profile=$(COV)/babysit.profdata \
	    -format=lcov $(COV_SOURCES) > $(COV)/lcov.info
	@awk '/^TOTAL/ { sub("%", "", $$10); \
	    printf "Line coverage: %s%% (minimum $(COVERAGE_MIN)%%)\n", $$10; \
	    exit ($$10 + 0 < $(COVERAGE_MIN)) }' $(COV)/report.txt

# The same targets on FreeBSD, in a throwaway QEMU VM (see the script's
# --help). GitLab has no hosted FreeBSD runners, so this is run by hand.
check-freebsd-vm:
	bash tests/freebsd-vm.sh

# libFuzzer targets with AddressSanitizer, on a small RAM disk that the run
# fills up (see tests/fuzz/run.sh --help). Needs clang.
FUZZ_SECONDS ?= 60
fuzz:
	bash tests/fuzz/run.sh --seconds $(FUZZ_SECONDS)

lint:
	shellcheck -x --severity=warning tests/fuzz/run.sh tests/run.sh tests/lib.sh \
	    tests/freebsd-vm.sh tests/ci-local.sh tests/testcases/*-test.sh \
	    tests/perf.sh tests/perflib.sh tests/perfcases/*-test.sh \
	    completions/babysit.bash \
	    precommit.sh
	@for m in $(MANPAGES); do \
	    out=$$(groff -man -Tutf8 -ww -z $$m 2>&1); \
	    if [ -n "$$out" ]; then echo "$$m: $$out"; exit 1; fi; \
	done; echo "man pages: ok"

# ---- install -------------------------------------------------------------

# Two ways to install, and they are different places, so pick one:
#
#   sudo make install     for all users, under PREFIX (/usr/local)
#   make install-user     for yourself, under $HOME/.local, no sudo
#
# Running one and then the other leaves two copies; "sudo make uninstall"
# or "make uninstall-user" removes one again.
USER_INSTALL = PREFIX=$(HOME)/.local UNITDIR=$(HOME)/.config/systemd/user

install-user:
	$(MAKE) $(USER_INSTALL) install

uninstall-user:
	$(MAKE) $(USER_INSTALL) uninstall

install: all
	@d="$(DESTDIR)$(PREFIX)"; \
	while [ ! -e "$$d" ] && [ "$$d" != "$${d%/*}" ]; do d="$${d%/*}"; done; \
	[ -n "$$d" ] || d=/; \
	if [ ! -w "$$d" ]; then \
	    echo "make install: you cannot write to $$d."; \
	    echo "  to install for all users:  sudo make install"; \
	    echo "  to install for yourself:   make install-user"; \
	    exit 1; \
	fi
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(MANDIR)/man1 \
	    $(DESTDIR)$(MANDIR)/man5 $(DESTDIR)$(MANDIR)/man8 $(DESTDIR)$(DOCDIR)
	# Copy beside, then rename into place: a babysitter started from the
	# installed binary may be running, and a file that is being executed
	# cannot be written over, only replaced.
	for p in $(PROGS); do \
	    cp $$p $(DESTDIR)$(PREFIX)/bin/.$${p##*/}.new && \
	    mv -f $(DESTDIR)$(PREFIX)/bin/.$${p##*/}.new \
	        $(DESTDIR)$(PREFIX)/bin/$${p##*/} || exit 1; \
	done
	cp docs/babysit.1 $(DESTDIR)$(MANDIR)/man1/
	cp docs/babysit.conf.5 $(DESTDIR)$(MANDIR)/man5/
	cp docs/babysitd.8 $(DESTDIR)$(MANDIR)/man8/
	# The unit names the installed babysitd, so it is written here, from
	# the PREFIX of this install, and not at build time: a unit built with
	# one PREFIX and installed with another points at a file that is not
	# there, and the service fails to start.
	if [ -n "$(UNITDIR)" ]; then mkdir -p $(DESTDIR)$(UNITDIR) && \
	    sed 's|@BINDIR@|$(PREFIX)/bin|' src/babysitd/babysitd.service.in \
	    > $(DESTDIR)$(UNITDIR)/babysitd.service; fi
	# One completion file serves both commands; bash-completion finds it
	# by the name of the command being completed, so it goes in twice.
	if [ -n "$(COMPDIR)" ]; then mkdir -p $(DESTDIR)$(COMPDIR) && \
	    cp completions/babysit.bash $(DESTDIR)$(COMPDIR)/babysit && \
	    cp completions/babysit.bash $(DESTDIR)$(COMPDIR)/babysitd; fi
	cp src/babysitd/babysit.conf README.md LICENSE $(DESTDIR)$(DOCDIR)/

# Remove what "make install" with the same PREFIX (and UNITDIR, COMPDIR)
# put there, file by file. Logs, your configuration file and a running
# service are not touched.
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/babysit $(DESTDIR)$(PREFIX)/bin/babysitd
	rm -f $(DESTDIR)$(MANDIR)/man1/babysit.1 \
	    $(DESTDIR)$(MANDIR)/man5/babysit.conf.5 \
	    $(DESTDIR)$(MANDIR)/man8/babysitd.8
	rm -f $(DESTDIR)$(DOCDIR)/babysit.conf $(DESTDIR)$(DOCDIR)/README.md \
	    $(DESTDIR)$(DOCDIR)/LICENSE
	-rmdir $(DESTDIR)$(DOCDIR) 2>/dev/null
	if [ -n "$(UNITDIR)" ]; then \
	    rm -f $(DESTDIR)$(UNITDIR)/babysitd.service; fi
	if [ -n "$(COMPDIR)" ]; then \
	    rm -f $(DESTDIR)$(COMPDIR)/babysit $(DESTDIR)$(COMPDIR)/babysitd; fi

# The skill that teaches a coding agent to use babysit (skills/babysit/).
# It is copied, so the installed skill does not depend on this checkout; pass
# SKILL_LINK=1 to link to it instead, which suits working on the skill, since
# edits here then take effect in the next session. A new session is needed
# either way: skills are read when a session starts.
install-claude-skill:  SKILLDEST = $(CLAUDE_SKILLDIR)
install-copilot-skill: SKILLDEST = $(COPILOT_SKILLDIR)
install-claude-skill install-copilot-skill:
	@mkdir -p "$(SKILLDEST)"
	@rm -rf "$(SKILLDEST)/babysit"
	@if [ -n "$(SKILL_LINK)" ]; then \
	    ln -s "$(CURDIR)/skills/babysit" "$(SKILLDEST)/babysit"; \
	    echo "linked $(SKILLDEST)/babysit -> $(CURDIR)/skills/babysit"; \
	else \
	    cp -R skills/babysit "$(SKILLDEST)/babysit"; \
	    echo "installed $(SKILLDEST)/babysit"; \
	fi
	@command -v babysit >/dev/null || \
	    echo "note: babysit is not on your PATH yet; the skill only helps once it is (make install PREFIX=\$$HOME/.local)"

# ---- packages ------------------------------------------------------------

# Source tarball: build/babysit-VERSION.tar.gz
dist:
	@grep -q '^babysit ($(VERSION))' debian/changelog || \
	    { echo "debian/changelog does not start with version $(VERSION)"; exit 1; }
	rm -rf $(BUILD)/babysit-$(VERSION)
	mkdir -p $(BUILD)/babysit-$(VERSION)
	cp -R $(DISTFILES) $(BUILD)/babysit-$(VERSION)/
	tar -C $(BUILD) -czf $(BUILD)/babysit-$(VERSION).tar.gz babysit-$(VERSION)
	rm -rf $(BUILD)/babysit-$(VERSION)
	@echo "$(BUILD)/babysit-$(VERSION).tar.gz"

# Binary tarball: build/babysit-VERSION-linux-ARCH.tar.gz, rooted at /usr/local
bindist:
	rm -rf $(BUILD)/bindist
	$(MAKE) BINDIR=$(BUILD)/bindist/obj PREFIX=/usr/local \
	    DESTDIR=$(CURDIR)/$(BUILD)/bindist/root install
	tar -C $(BUILD)/bindist/root -czf \
	    $(BUILD)/babysit-$(VERSION)-linux-$(ARCH).tar.gz usr
	rm -rf $(BUILD)/bindist
	@echo "$(BUILD)/babysit-$(VERSION)-linux-$(ARCH).tar.gz"

# build/rpm/RPMS/*/babysit-VERSION-*.rpm (needs rpmbuild)
rpm: dist
	sed 's/@VERSION@/$(VERSION)/' packaging/babysit.spec.in > $(BUILD)/babysit.spec
	rpmbuild -bb --define "_topdir $(CURDIR)/$(BUILD)/rpm" \
	    --define "_sourcedir $(CURDIR)/$(BUILD)" $(BUILD)/babysit.spec

# build/deb/babysit_VERSION_*.deb (needs dpkg-buildpackage and debhelper)
deb: dist
	rm -rf $(BUILD)/deb
	mkdir -p $(BUILD)/deb
	tar -C $(BUILD)/deb -xzf $(BUILD)/babysit-$(VERSION).tar.gz
	cd $(BUILD)/deb/babysit-$(VERSION) && dpkg-buildpackage -b -us -uc

# build/arch/babysit-VERSION-1-ARCH.pkg.tar.zst, on Arch Linux (needs
# makepkg, which refuses to run as root)
arch-pkg: dist
	rm -rf $(BUILD)/arch
	mkdir -p $(BUILD)/arch
	cp $(BUILD)/babysit-$(VERSION).tar.gz $(BUILD)/arch/
	sum=$$(sha256sum $(BUILD)/babysit-$(VERSION).tar.gz | cut -d ' ' -f 1); \
	    sed "s/@VERSION@/$(VERSION)/; s/@SHA256@/$$sum/" \
	    packaging/arch/PKGBUILD.in > $(BUILD)/arch/PKGBUILD
	cd $(BUILD)/arch && makepkg -f --noconfirm

# build/babysit-VERSION.pkg, on FreeBSD (needs pkg; run with gmake)
freebsd-pkg:
	rm -rf $(BUILD)/pkg
	$(MAKE) BINDIR=$(BUILD)/pkg/obj PREFIX=/usr/local UNITDIR= \
	    DESTDIR=$(CURDIR)/$(BUILD)/pkg/stage install
	sed 's/@VERSION@/$(VERSION)/' packaging/freebsd/manifest.in \
	    > $(BUILD)/pkg/+MANIFEST
	cd $(BUILD)/pkg/stage && find usr -type f | sed 's|^|/|' > ../plist
	pkg create -M $(BUILD)/pkg/+MANIFEST -r $(BUILD)/pkg/stage \
	    -p $(BUILD)/pkg/plist -o $(BUILD)

clean:
	rm -rf bin $(BUILD)

.PHONY: all test check perf check-asan check-ubsan check-tsan check-freebsd-vm \
        coverage fuzz lint \
        install install-user uninstall uninstall-user \
        install-claude-skill install-copilot-skill dist bindist rpm deb arch-pkg freebsd-pkg clean
