#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Invalid number of arguments."
    echo "Usage: make_rpm.sh <builddir> <version>"
    exit 1
fi

BUILDDIR=$1
VERSION=$2

meson dist -C "$BUILDDIR" --no-tests
rpmbuild --define="_topdir $PWD/rpms" \
    -ta "$BUILDDIR/meson-dist/lhsmtool_phobos-$VERSION.tar.xz"
