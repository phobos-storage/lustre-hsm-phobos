VERSION=0.1.0
mkfile_path := $(abspath $(lastword $(MAKEFILE_LIST)))
mkfile_dir := $(dir $(mkfile_path))

all: ioproxy.so

ioproxy.so: slurm-spank-ioproxy.c Makefile
	gcc -g -Wall -shared -fPIC -o  ioproxy.so slurm-spank-ioproxy.c -lmount

clean:
	rm -f *.o ioproxy.so *.tar.gz

new: clean all 

dist:
	rm -fr /tmp/slurm-spank-ioproxy-${VERSION} ; mkdir -p /tmp/slurm-spank-ioproxy-${VERSION} ; \
	cp slurm-spank-ioproxy.c slurm-spank-ioproxy.spec /tmp/slurm-spank-ioproxy-${VERSION} ;\
	cd /tmp; tar cvfz $(mkfile_dir)/slurm-spank-ioproxy-${VERSION}.tar.gz slurm-spank-ioproxy-${VERSION}/*

rpm: dist
	rpmbuild -ta slurm-spank-ioproxy-${VERSION}.tar.gz
