# patchelf is required to create any package containing elf files
if host_machine.system() == 'linux'
    find_program('patchelf', required : true)
endif

mmpack_build_sources = files(
        '__init__.py',
        '__main__.py',
        'base_hook.py',
        'binary_package.py',
        'common.py',
        'decorators.py',
        'elf_utils.py',
        'file_utils.py',
        'hook_locales.py',
        'hook_python.py',
        'hook_sharedlib.py',
        'hooks_loader.py',
        'mmpack_builddep.py',
        'mmpack_clean.py',
        'mmpack_guess.py',
        'mmpack_pkg_create.py',
        'package_info.py',
        'pe_utils.py',
        'provide.py',
        'python_depends.py',
        'python_provides.py',
        'source_tarball.py',
        'src_package.py',
        'syspkg_manager.py',
        'syspkg_manager_base.py',
        'syspkg_manager_dpkg.py',
        'syspkg_manager_pacman_msys2.py',
        'mm_version.py',
        'workspace.py',
        'xdg.py',
)

mmpackbuilddir = python.get_install_dir() / 'mmpackbuild'

bindir = get_option('prefix') / get_option('bindir')
r = run_command('realpath', '-m', '--relative-to=' + mmpackbuilddir, bindir)
pydir_to_bindir = r.stdout().strip()

libexecdir = get_option('prefix') / get_option('libexecdir')
r = run_command('realpath', '-m', '--relative-to=' + mmpackbuilddir, libexecdir)
pydir_to_libexecdir = r.stdout().strip()

pkgdatadir = get_option('prefix') +'/'+ get_option('datadir') + '/mmpack'
r = run_command('realpath', '-m', '--relative-to=' + mmpackbuilddir, pkgdatadir)
pydir_to_pkgdatadir = r.stdout().strip()

config_data = configuration_data({
    'bindir' : bindir,
    'pydir_to_bindir' :  pydir_to_bindir,
    'pydir_to_libexecdir' : pydir_to_libexecdir,
    'pydir_to_pkgdatadir' : pydir_to_pkgdatadir,
    'python_install_dir' : python.get_install_dir(),
    'package_version' : meson.project_version(),
    'exeext' : exeext,
})

settings_py = configure_file(
        output : 'settings.py',
        input : files('settings.py.in'),
        configuration : config_data,
)
mmpack_build_sources += settings_py

python.install_sources(mmpack_build_sources, subdir : 'mmpack_build')

mmpack_build = configure_file(
        output : 'mmpack-build',
        input : 'mmpack-build.in',
        configuration : config_data,
)
install_data(mmpack_build,
    install_dir : get_option('bindir'),
)

mmpack_build_plugins = files(
	'build-autotools',
	'build-cmake',
	'build-makefile',
	'build-meson',
	'build-python',
)
install_data(mmpack_build_plugins,
    install_dir : get_option('datadir') / 'mmpack',
)
