# @mindmaze_header@
'''
TODOC
'''

import logging
import logging.handlers
import os
import sys
from subprocess import PIPE, run
from typing import Union
from hashlib import sha256
import yaml
from decorators import run_once
from xdg.BaseDirectory import xdg_data_home

CONFIG = {'debug': True, 'verbose': True}


@run_once
def _init_logger_if_not():
    '''
    Init logger to print to *log_file*.
    Usually called via (e|i|d)print helpers.

    If verbose flag is set, it will log up to DEBUG level
    otherwise, only up to INFO level.
    '''
    log_file = xdg_data_home + '/mmpack.log'
    log_handler = logging.handlers.TimedRotatingFileHandler(log_file)
    logger = logging.getLogger()
    logger.addHandler(log_handler)

    if CONFIG['debug']:
        logger.setLevel(logging.DEBUG)
    elif CONFIG['debug'] or CONFIG['verbose']:
        logger.setLevel(logging.INFO)
    else:
        logger.setLevel(logging.WARNING)


def eprint(*args, **kwargs):
    'error print: print to stderr'
    _init_logger_if_not()
    logging.error(*args, **kwargs)
    print(*args, file=sys.stderr, **kwargs)


def iprint(*args, **kwargs):
    'info print: print only if verbose flag is set'
    _init_logger_if_not()
    logging.info(*args, **kwargs)
    if CONFIG['verbose'] or CONFIG['debug']:
        print(*args, file=sys.stderr, **kwargs)


def dprint(*args, **kwargs):
    'debug print: standard print and log'
    _init_logger_if_not()
    logging.debug(*args, **kwargs)
    if CONFIG['debug']:
        print(*args, file=sys.stderr, **kwargs)


class ShellException(RuntimeError):
    'custom exception for shell command error'


def shell(cmd):
    'Wrapper for subprocess.run'
    dprint('[shell] {0}'.format(cmd))
    ret = run(cmd, stdout=PIPE, shell=True)
    if ret.returncode == 0:
        return ret.stdout.decode('utf-8')
    else:
        raise ShellException('failed to exec command')


# initialise a directory stack
PUSHSTACK = []


def pushdir(dirname):
    'Save and then change the current directory'
    PUSHSTACK.append(os.getcwd())
    os.chdir(dirname)


def popdir(num=1):
    'Remove the top n entries from the directory stack'
    os.chdir(PUSHSTACK.pop())
    if num > 1:
        popdir(num - 1)


def git_root():
    'Get the git top directory'
    shell('git rev-parse --show-toplevel')


def remove_duplicates(lst):
    'remove duplicates from list (in place)'
    for elt in lst:
        while lst and lst.count(elt) > 1:
            lst.remove(elt)


def filetype(filename):
    'get file type'
    file_type = shell('file  --brief --preserve-date {}'.format(filename))
    # try to read file type first
    if file_type.startswith('ELF '):
        return 'elf'
    elif file_type.startswith('pe'):
        return 'pe'

    # return file extension otherwise
    # eg. python, c-header, ...
    return os.path.splitext(filename)[1][1:].strip().lower()


def yaml_serialize(obj: Union[list, dict], filename: str) -> None:
    'Save object as yaml file of given name'
    with open(filename, 'w+') as outfile:
        yaml.dump(obj, outfile,
                  default_flow_style=False,
                  allow_unicode=True,
                  indent=4)
    dprint('wrote {0}'.format(filename))


def mm_representer(dumper, data):
    '''
    enforce yaml interpretation of given complex object type as unicode
    classes which want to benefit must add themselves as follow:
      yaml.add_representer(<class-name>, mm_representer)

    (Otherwise, they will be printed with a !!python/object tag)
    '''
    return dumper.represent_data(repr(data))


def sha256sum(filename: str) -> str:
    ''' compute the SHA-256 hash a file

    Args:
        filename: path of file whose hash must be computed
    Returns:
        a string containing hexadecimal value of hash
    '''
    sha = sha256()
    sha.update(open(filename, 'rb').read())
    return sha.hexdigest()
