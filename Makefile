VERSION=0.1.0

all: lhsmtool_phobos lhsmtool_posix
CFLAGS=-g -Wall -Werror
INC=-I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include  -I/usr/include/glib-2.0 -I/usr/lib64/glib-2.0/include
lhsmtool_phobos: lhsmtool_phobos.c Makefile
	gcc $(CFLAGS) $(INC)  -o  lhsmtool_phobos lhsmtool_phobos.c -lrt -lglib-2.0 -pthread -lgthread-2.0 -llustreapi  -lphobos_store

lhsmtool_posix: lhsmtool_posix.c Makefile
	gcc $(CFLAGS) $(INC)  -o  lhsmtool_posix lhsmtool_posix.c -lrt -lglib-2.0 -pthread -lgthread-2.0 -llustreapi

clean:
	rm -f *.o lhsmtool_phobos lhsm_posix *.tar.gz

new: clean all 

