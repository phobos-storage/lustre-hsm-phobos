VERSION=1.0
NAME=lhsmtool_phobos
DISTDIR=$(NAME)-$(VERSION)

ifeq ($(PREFIX),)
	PREFIX := /usr
endif

all: \
	build \
	build/lhsmtool_phobos \
	lhsmtool_phobos.spec

glib_LDFLAGS=$(shell pkg-config --libs glib-2.0)
glib_CFLAGS=$(shell pkg-config --cflags glib-2.0)

SOURCES=lhsmtool_phobos.c src/layout.c
HEADERS=src/layout.h
TESTS=tests/hsm_import.c

CFLAGS=-g -Wall -Werror -Isrc
LDFLAGS=-pthread -l:liblustreapi.a

build:
	mkdir -p build

build/lhsmtool_phobos: $(HEADERS) $(SOURCES) Makefile build
	$(CC) $(CFLAGS) $(glib_CFLAGS) -o $@ $(SOURCES) \
		$(LDFLAGS) $(glib_LDFLAGS) -lphobos_store

build/lhsmtool_posix: lhsmtool_posix.c Makefile build
	$(CC) $(CFLAGS) -o $@ lhsmtool_posix.c -lrt $(LDFLAGS)

build/hsm-import: tests/hsm_import.c Makefile build
	$(CC) $(CFLAGS) -o $@ tests/hsm_import.c $(LDFLAGS)

lhsmtool_phobos.spec: lhsmtool_phobos.spec.in Makefile
	cp $< $@
	sed -i 's/Version: @VERSION@/Version: $(VERSION)/' $@

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 build/lhsmtool_phobos $(DESTDIR)$(PREFIX)/bin

clean:
	rm -f build/* *.tar.gz

dist: all
	mkdir -p $(DISTDIR)
	cp lhsmtool_phobos.c $(DISTDIR)
	cp lhsmtool_phobos.spec $(DISTDIR)
	cp lhsmtool_phobos.spec.in $(DISTDIR)
	cp Makefile $(DISTDIR)
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
