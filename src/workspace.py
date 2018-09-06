# @mindmaze_header@
'''
os helpers to manipulate the paths and environments
'''

import os
import sys
from xdg.BaseDirectory import xdg_config_home, xdg_cache_home, xdg_data_home

from decorators import singleton
from common import shell


@singleton
class Workspace(object):
    'global mmpack workspace singleton class'
    def __init__(self):
        self.config = xdg_config_home + '/mmpack-config.yml'
        self.sources = xdg_cache_home + '/mmpack-sources'
        self.build = xdg_cache_home + '/mmpack-build'
        self.packages = xdg_data_home + '/mmpack-packages'

        # create the directories if they do not exist
        os.makedirs(xdg_config_home, exist_ok=True)
        os.makedirs(self.build, exist_ok=True)
        os.makedirs(self.sources, exist_ok=True)
        os.makedirs(self.packages, exist_ok=True)

    def builddir(self, pkg):
        'get package build directory. Create it if needed.'

        builddir = self.build + '/' + pkg
        os.makedirs(builddir, exist_ok=True)
        return builddir

    def clean(self, pkg=None):
        '''remove all copied sources and temporary build objects
           keep generated packages.
           if pkg is explicited, will only clean given package
        '''
        if not pkg:
            pkg = ''

        shell('rm -rvf {0}/{1}*'.format(self.build, pkg))
        shell('rm -rvf {0}/{1}*'.format(self.sources, pkg))
        shell('rm -rvf {0}/{1}*'.format(self.packages, pkg))

    def wipe(self):
        'same as clean, but also remove all created pacakges'
        self.clean()
        shell('rm -vf {0}/*'.format(self.packages))


def is_valid_prefix(prefix: str) -> bool:
    'returns wether given prefix is a valid path for mmpack prefix'
    return os.path.exists(prefix + '/var/lib/mmpack/')


def push_prefix(prefix: str) -> None:
    ''' Prepend given prefix to all relevant os environment variables
        Since python is launched in its own process, directly write to
        os.environ.
    '''
    # system environment execution
    os.environ['PATH'] = '{0}/bin:{1}'.format(prefix, os.environ.get('PATH'))
    os.environ['LD_LIBRARY_PATH'] = '{0}/lib:{1}' \
                                    .format(prefix,
                                            os.environ.get('LD_LIBRARY_PATH'))
    os.environ['LIBRARY_PATH'] = '{0}/lib:{1}' \
                                 .format(prefix,
                                         os.environ.get('LIBRARY_PATH'))
    os.environ['CPATH'] = '{0}/include:{1}' \
                          .format(prefix, os.environ.get('CPATH'))

    pythonpath = '/lib/python/{:d}.{:d}'.format(sys.version_info.major,
                                                sys.version_info.minor)
    os.environ['PYTHONPATH'] = '{0}/{1}:{2}' \
                               .format(prefix, pythonpath,
                                       os.environ.get('PYTHONPATH'))
