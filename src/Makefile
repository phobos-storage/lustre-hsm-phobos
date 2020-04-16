VERSION=0.1.0

all: lhsmtool_phobos

lhsmtool_phobos: lhsmtool_phobos.c Makefile
	gcc -g -Wall -I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include -pthread -I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include -o  lhsmtool_phobos lhsmtool_phobos.c -lrt -lglib-2.0 -pthread -lgthread-2.0 -llustreapi

clean:
	rm -f *.o lhsmtool_phobos *.tar.gz

new: clean all 

