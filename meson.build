# Copyright (C) 2023 Commissariat a l'energie atomique et aux energies
#                    alternatives
#
# SPDX-License-Identifer: GPL-2.0-only

project(
    'lhsmtool_phobos',
    'c',
    version: '1.1',
    license: 'GPL-2.0-only',
    default_options: [
        'werror=true',
        'c_std=c11',
        'warning_level=3',
    ],
)

add_project_arguments(
    [ '-DHAVE_CONFIG_H', '-Wno-pedantic' ],
    language: 'c'
)

cc = meson.get_compiler('c')

phobos_store = cc.find_library('phobos_store', required: true)
phobos_admin = cc.find_library('phobos_admin', required: true)
lustre = cc.find_library('lustreapi', required: true)
glib2 = dependency('glib-2.0', required: true)
pthread = dependency('threads', required: true)

have_phobos_init = cc.has_function(
    'phobos_init',
    prefix: '#define _GNU_SOURCE\n#include <phobos_store.h>',
    dependencies: [ glib2, phobos_store ],
)
have_layout_set_by_fd = cc.has_function(
    'llapi_layout_set_by_fd',
    prefix: '#include <lustre/lustreapi.h>',
    dependencies: lustre,
)

config = configuration_data()
config.set('HAVE_LLAPI_LAYOUT_SET_BY_FD', have_layout_set_by_fd)
config.set('HAVE_PHOBOS_INIT', have_phobos_init)

configure_file(
    input: 'config.h.in',
    output: 'config.h',
    configuration: config,
)

libcopytool = static_library(
    'copytool',
    sources: [
        'src/layout.c',
        'src/hints.c',
        'src/log.c',
        'src/phobos.c',
    ],
    dependencies: [
        phobos_store,
        phobos_admin,
        glib2,
        lustre,
        pthread,
    ],
    include_directories: include_directories('.', 'src')
)

copytool_dependency = declare_dependency(
    link_with: libcopytool,
    include_directories: include_directories('.', 'src')
)

executable(
    'lhsmtool_phobos',
    sources: [
        'lhsmtool_phobos.c',
    ],
    include_directories: include_directories('.', 'src'),
    dependencies: [ glib2, copytool_dependency ],
    install: true,
)

specfile = configuration_data()
specfile.set('version', meson.project_version())
configure_file(
    input: 'lhsmtool_phobos.spec.in',
    output: 'lhsmtool_phobos.spec',
    configuration: specfile,
)

meson.add_dist_script('utils/add_specfile_to_dist.sh')
custom_target('rpm',
    output: 'lhsmtool_phobos-' + meson.project_version() + '.rpm',
    command: [
        '../utils/make_rpm.sh', meson.current_build_dir(), meson.project_version()
    ],
)

executable(
    'hsm-import',
    sources: 'tests/hsm_import.c',
    dependencies: lustre,
)
acceptance = find_program('acceptance.sh', required: true)

# FIXME for some reason, the tests fail when run using ninja but still pass if
# acceptance is run by hand.
# test('acceptance', acceptance)

cmocka = dependency('cmocka', required: false)

if cmocka.found()
    unit = executable(
        'unit',
        sources: 'tests/unit.c',
        dependencies: [ glib2, cmocka, copytool_dependency ]
    )
    test('unit', unit)
endif
