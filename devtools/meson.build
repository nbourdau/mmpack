install_data(files('mmpack-build-slave.sh'),
        install_dir : get_option('bindir'),
        install_mode: 'rwxr-xr-x'
)

mmpack_build_gerrit_files = files(
        'gerrit/gerrit-mmpack-build.py',
        'gerrit/gerrit.py',
)
install_data(mmpack_build_gerrit_files,
    install_dir : get_option('datadir') / 'mmpack/gerrit/python',
)


#
# dev targets
#

all_sources = (mmpack_sources
        + mmpack_build_sources
        + mmpack_build_plugins
        + mmpack_check_sysdep_sources
       + mount_mmpack_prefix_sources)

if get_option('tests')
    all_sources += mmpack_unit_tests_sources
endif

if get_option('doc')
    all_sources += man_pages_sources
endif

codespell = find_program('codespell', required : false)
if codespell.found()
    run_target('spelling',
        command : [
            codespell,
            all_sources,
        ]
    )
endif # codespell