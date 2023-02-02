VERSION=1.1
NAME=lhsmtool_phobos
DISTDIR=$(NAME)-$(VERSION)

ifeq ($(PREFIX),)
	PREFIX := /usr
endif

ifeq ($(SYSCONFDIR),)
	SYSCONFDIR := /etc
endif

all: \
	build \
	build/lhsmtool_phobos \
	lhsmtool_phobos.spec

glib_LDFLAGS=$(shell pkg-config --libs glib-2.0)
glib_CFLAGS=$(shell pkg-config --cflags glib-2.0)

SOURCES=lhsmtool_phobos.c src/layout.c src/log.c
HEADERS=src/layout.h src/common.h
TESTS=tests/hsm_import.c

CFLAGS=-g -Wall -Wextra -Werror -Isrc
LDFLAGS=-pthread -llustreapi

build:
	mkdir -p build

build/lhsmtool_phobos: $(HEADERS) $(SOURCES) Makefile build
	$(CC) $(CFLAGS) $(glib_CFLAGS) -o $@ $(SOURCES) \
		$(LDFLAGS) $(glib_LDFLAGS) -lphobos_store

build/hsm-import: tests/hsm_import.c Makefile build
	$(CC) $(CFLAGS) -o $@ tests/hsm_import.c $(LDFLAGS)

lhsmtool_phobos.spec: lhsmtool_phobos.spec.in Makefile
	cp $< $@
	sed -i 's/Version: @VERSION@/Version: $(VERSION)/' $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 build/lhsmtool_phobos $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(shell pkg-config systemd --variable=systemdsystemunitdir)
	install -m 0544 lhsmtool_phobos.service \
		$(DESTDIR)$(shell pkg-config systemd --variable=systemdsystemunitdir)
	install -d $(DESTDIR)$(SYSCONFDIR)/sysconfig
	install -m 0644 lhsmtool_phobos.conf $(DESTDIR)$(SYSCONFDIR)/sysconfig/

clean:
	rm -f build/* *.tar.gz

dist: all
	mkdir -p $(DISTDIR)
	cp lhsmtool_phobos.c $(DISTDIR)
	cp -R src $(DISTDIR)
	cp lhsmtool_phobos.spec $(DISTDIR)
	cp lhsmtool_phobos.spec.in $(DISTDIR)
	cp Makefile $(DISTDIR)
	cp systemd/lhsmtool_phobos.service $(DISTDIR)
	cp systemd/lhsmtool_phobos.conf $(DISTDIR)
	tar -zcvf $(DISTDIR).tar.gz $(DISTDIR)
	rm -rf $(DISTDIR)

RPMDIR=`pwd`/rpms

rpm: dist lhsmtool_phobos.spec
	rpmbuild --define="_topdir $(RPMDIR)" -ta lhsmtool_phobos-$(VERSION).tar.gz

check: all build/hsm-import
	@bash acceptance.sh

checkpatch:
	./checkpatch.pl --no-tree -f $(SOURCES) $(HEADERS) $(TESTS)
	./checkpatch.pl --no-tree -f acceptance.sh

.PHONY: all clean checkpatch dist rpm install check
