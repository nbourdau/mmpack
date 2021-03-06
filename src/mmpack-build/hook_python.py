# @mindmaze_header@
"""
plugin tracking containing python file handling functions
"""

import filecmp
import os
import re
import shutil
from collections import namedtuple
from glob import glob
from typing import Set, Dict, List

from . base_hook import BaseHook
from . common import shell, Assert
from . file_utils import is_python_script
from . package_info import PackageInfo, DispatchData
from . provide import Provide, ProvideList, load_mmpack_provides
from . syspkg_manager import get_syspkg_mgr


# example of matches:
# 'lib/python3.6/site-packages/foo.so'
#               => (lib/python3.6/site-packages, foo, .so)
# 'lib/python3/site-packages/_foo.so'
#               => (lib/python3/site-packages, foo, .so)
# 'mingw64/lib/python3/site-packages/_foo.so'
#               => (mingw64/lib/python3/site-packages, foo, .so)
# 'usr/lib/python3/dist-packages/_foo.so'
#               => (usr/lib/python3/dist-packages, foo, .so)
# 'lib/python3/site-packages/foo_bar.py'
#               => (lib/python3/site-packages, foo_bar, .py)
# 'lib/python3/site-packages/foo/__init__.py'
#               => (lib/python3/site-packages, foo, None)
# 'lib/python3/site-packages/foo/_internal.so'
#               => (lib/python3/site-packages, foo, None)
# 'lib/python3/site-packages/Foo-1.2.3.egg-info/_internal.so'
#               => (lib/python3/site-packages, foo, -1.2.3-egg-info)
# 'lib/python2/site-packages/foo.so'
#               => None
_PKG_REGEX = re.compile(
    r'((?:usr/|mingw64/)?lib/python3(?:\.\d)?/(?:dist|site)-packages)'
    r'/_?([\w_]+)([^/]*)'
)


# location relative to the prefix dir where the files of public python packages
# must be installed
_MMPACK_REL_PY_SITEDIR = 'lib/python3/site-packages'


PyNameInfo = namedtuple('PyNameInfo', ['pyname', 'sitedir', 'is_egginfo'])


def _parse_py3_filename(filename: str) -> PyNameInfo:
    """
    Get the python3 package name a file should belongs to (ie the one with
    __init__.py file if folder, or the name of the single module if directly in
    site-packages folder) along with the site dir that the package belongs to.

    Args:
        filename: file to test

    Return: NamedTuple(pyname, sitedir, is_egginfo)

    Raises:
        FileNotFoundError: the file does not belong to a public python package
    """
    res = _PKG_REGEX.search(filename)
    if not res:
        raise FileNotFoundError

    grps = res.groups()
    is_egg = grps[2].endswith('.egg-info')
    return PyNameInfo(pyname=grps[1], sitedir=grps[0], is_egginfo=is_egg)


def _mmpack_pkg_from_pyimport_name(pyimport_name: str):
    """
    Return the name of the mmpack package that should provide the
    `pyimport_name` passed in argument.
    """
    return 'python3-' + pyimport_name.lower()


def _gen_pysymbols(pyimport_name: str, pkg: PackageInfo,
                   sitedir: str) -> Set[str]:
    script = os.path.join(os.path.dirname(__file__), 'python_provides.py')
    cmd = ['python3', script, '--site-path='+sitedir, pyimport_name]
    cmd += list(pkg.files)

    cmd_output = shell(cmd)
    return set(cmd_output.split())


def _gen_pydepends(pkg: PackageInfo, sitedir: str) -> Set[str]:
    script = os.path.join(os.path.dirname(__file__), 'python_depends.py')
    cmd = ['python3', script, '--site-path='+sitedir]
    cmd += [f for f in pkg.files if is_python_script(f)]

    cmd_output = shell(cmd)
    return set(cmd_output.split())


#####################################################################
# Python hook for mmpack-build
#####################################################################

class MMPackBuildHook(BaseHook):
    """
    Hook tracking python module used and exposed
    """
    def __init__(self, srcname: str, host_archdist: str, description: str):
        super().__init__(srcname, host_archdist, description)
        self._mmpack_py_provides = None

    def _get_mmpack_provides(self) -> ProvideList:
        """
        Get all shared library soname and associated symbols for all mmpack
        package installed in prefix. The parsing of all .symbols files is
        cached, hence subsequent calls to this method is very fast.
        """
        if not self._mmpack_py_provides:
            self._mmpack_py_provides = load_mmpack_provides('pyobjects',
                                                            'python')
        return self._mmpack_py_provides

    def _gen_py_deps(self, currpkg: PackageInfo, used_symbols: Set[str],
                     others_pkgs: List[PackageInfo]):
        """
        For each key (imported package name) in `imports` determine the mmpack
        or system dependency that provides it and add it to those of
        `currpkg`.

        Args:
            currpkg: package whose dependencies are computed (and added)
            used_symbols: py symbols used in currpkg
            others_pkgs: list of packages cobuilded (may include currpkg)
        """
        imports = {s.split('.', maxsplit=1)[0] for s in used_symbols}

        # provided in the same package or a package being generated
        for pkg in others_pkgs:
            dep_list = pkg.provides['python'].gen_deps(imports, used_symbols)
            for pkgname, _ in dep_list:
                currpkg.add_to_deplist(pkgname, pkg.version, pkg.version)

        # provided by another mmpack package present in the prefix
        dep_list = self._get_mmpack_provides().gen_deps(imports, used_symbols)
        for pkgname, version in dep_list:
            currpkg.add_to_deplist(pkgname, version)

        # provided by the host system
        syspkg_mgr = get_syspkg_mgr()
        for pypkg in imports:
            sysdep = syspkg_mgr.find_pypkg_sysdep(pypkg)
            if not sysdep:
                # <pypkg> dependency could not be met with any available means
                errmsg = 'Could not find package providing {} python package'\
                         .format(pypkg)
                raise Assert(errmsg)

            currpkg.add_sysdep(sysdep)

    def post_local_install(self):
        """
        install move python3 packages from versioned python3 install folder
        to a unversioned python3 folder. This way, python3 version can be
        upgraded (normally, only python3 standard library has to be
        installed in a version installed folder).
        """
        # Move all public package from unversioned python3 folder to an
        # unversioned one
        for pydir in glob('lib/python3.*/site-packages'):
            os.makedirs(_MMPACK_REL_PY_SITEDIR, exist_ok=True)
            for srcdir, dirs, files in os.walk(pydir):
                reldir = os.path.relpath(srcdir, pydir)
                dstdir = os.path.join(_MMPACK_REL_PY_SITEDIR, reldir)

                # Create the folders in unversioned python3 site package
                for dirpath in dirs:
                    os.makedirs(os.path.join(dstdir, dirpath), exist_ok=True)

                # Move files to unversioned python3 site package
                for filename in files:
                    src = os.path.join(srcdir, filename)
                    dst = os.path.join(dstdir, filename)

                    # If destination file exists, we have no issue if source
                    # and destination are the same
                    if os.path.lexists(dst):
                        if filecmp.cmp(src, dst):
                            continue
                        raise FileExistsError

                    os.replace(src, dst)

            # Remove the remainings
            shutil.rmtree(pydir)

    def dispatch(self, data: DispatchData):
        for file in data.unassigned_files.copy():
            try:
                info = _parse_py3_filename(file)
                mmpack_pkgname = _mmpack_pkg_from_pyimport_name(info.pyname)
                pkg = data.assign_to_pkg(mmpack_pkgname, {file})
                if pkg.description:
                    continue

                pkg.description = '{}\nThis contains the python3 package {}'\
                                  .format(self._src_description, info.pyname)
            except FileNotFoundError:
                pass

    def update_provides(self, pkg: PackageInfo,
                        specs_provides: Dict[str, Dict]):
        py3_provides = ProvideList('python')

        py3pkgs = {}
        for instfile in pkg.files:
            try:
                info = _parse_py3_filename(instfile)
                if info.is_egginfo:
                    continue
                data = py3pkgs.setdefault(info.pyname, (info.sitedir, set()))
                data[1].add(instfile)
            except FileNotFoundError:
                pass

        # Loop over all python modules contained in the mmpack package and
        # parse the python public entry point of it (ie the entry point of the
        # python package)
        for pyname, data in py3pkgs.items():
            sitedir = data[0]
            py_files = data[1]
            root = '{}/{}'.format(sitedir, pyname)
            if not {root+'/__init__.py', root+'.py'}.isdisjoint(py_files):
                symbols = _gen_pysymbols(pyname, pkg, sitedir)
            else:
                raise RuntimeError('Not entry point found for python '
                                   'package {} in {}'
                                   .format(pyname, sitedir))

            provide = Provide(pyname)
            provide.pkgdepends = _mmpack_pkg_from_pyimport_name(pyname)
            provide.add_symbols(symbols, pkg.version)
            py3_provides.add(provide)

        # update symbol information from .provides file if any
        py3_provides.update_from_specs(specs_provides, pkg.name)

        pkg.provides['python'] = py3_provides

    def store_provides(self, pkg: PackageInfo, folder: str):
        filename = '{}/{}.pyobjects'.format(folder, pkg.name)
        pkg.provides['python'].serialize(filename)

    def update_depends(self, pkg: PackageInfo, other_pkgs: List[PackageInfo]):
        # Ignore dependency finding if ghost package
        if pkg.ghost:
            return

        py_scripts = [f for f in pkg.files if is_python_script(f)]
        if not py_scripts:
            return

        used_symbols = _gen_pydepends(pkg, _MMPACK_REL_PY_SITEDIR)
        self._gen_py_deps(pkg, used_symbols, other_pkgs)
