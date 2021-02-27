#
# test jig for objectfs
#

from ctypes import *
import os, sys, random

class stat(Structure):
    _fields_ = [("st_dev", c_longlong),	# 0
                ("st_ino", c_longlong),	# 8
                ("st_nlink", c_longlong),	# 16
                ("st_mode", c_int),	# 24
                ("st_uid", c_int),	# 28
                ("st_gid", c_int),	# 32
                ("_pad0", c_char * 4),	# 36
                ("st_rdev", c_longlong),	# 40
                ("st_size", c_longlong),	# 48
                ("st_blksize", c_longlong),	# 56
                ("st_blocks", c_longlong),	# 64
                ("st_atime", c_longlong),	# 72
                ("_pad1", c_char * 8),	# 80
                ("st_mtime", c_longlong),	# 88
                ("_pad2", c_char * 8),	# 96
                ("st_ctime", c_longlong),	# 104
                ("_pad3", c_char * 32),	# 112 -> 144
                ]

class dirent(Structure):
    _fields_ = [("name", c_char * 64),	# 0
                ("_pad0", c_char * 24),	# 64
                ("st_mode", c_int),	# 88
                ("st_uid", c_int),	# 92
                ("st_gid", c_int),	# 96
                ("_pad1", c_char * 12),	# 100
                ("st_size", c_longlong),	# 112
                ("_pad2", c_char * 32),	# 120
                ("st_mtime", c_longlong),	# 152
                ("_pad3", c_char * 8),	# 160
                ("st_ctime", c_longlong),	# 168
                ("_pad4", c_char * 32),	# 176 -> 208
                ]

class statvfs(Structure):
    _fields_ = [("f_bsize", c_longlong),	# 0
                ("_pad0", c_char * 8),	# 8
                ("f_blocks", c_longlong),	# 16
                ("f_bfree", c_longlong),	# 24
                ("f_bavail", c_longlong),	# 32
                ("_pad1", c_char * 40),	# 40
                ("f_namemax", c_longlong),	# 80
                ("_pad2", c_char * 24),	# 88 -> 112
                ]

class fuse_file_info(Structure):
    _fields_ = [("flags", c_int),	# 0
                ("_pad0", c_char * 20),	# 4
                ("fh", c_longlong),	# 24
                ("_pad1", c_char * 8),	# 32 -> 40
                ]

class fuse_context(Structure):
    _fields_ = [("fuse", c_longlong),	# 0
                ("uid", c_int),	# 8
                ("gid", c_int),	# 12
                ("pid", c_int),	# 16
                ("_pad0", c_char * 12),	# 20
                ("umask", c_int),	# 32
                ("_pad1", c_char * 4),	# 36 -> 40
                ]

dir = os.getcwd()
lib = CDLL(dir + "/libobjfs.so")
assert lib

def xbytes(path):
    if sys.version_info > (3,0):
        return bytes(path, 'UTF-8')
    else:
        return bytes(path)

def mkfs(pfx):
    return lib.mkfs(pfx)

def init(pfx):
    return lib.initialize(pfx)

def getattr(path):
    sb = stat()
    retval = lib.fs_getattr(xbytes(path), byref(sb))
    return retval, sb

dir_max = 128
def readdir(path):
    des = (dirent * dir_max)()
    n = c_int(dir_max)
    val = lib.fs_readdir(xbytes(path), byref(n), byref(des), byref(null_fi))
    if val >= 0:
        return val, des[0:n.value]
    else:
        return val, []

def create(path, mode):
    retval = lib.fs_create(path, c_int(mode), byref(null_fi))
    return retval

def mkdir(path, mode):
    retval = lib.fs_mkdir(path, c_int(mode))
    return retval

def truncate(path, offset):
    retval = lib.fs_truncate(path, c_int(offset))
    return retval

def unlink(path):
    retval = lib.fs_unlink(path)
    return retval

def rmdir(path):
    retval = lib.fs_rmdir(path)
    return retval

def rename(path1, path2):
    retval = lib.fs_rename(path1, path2)
    return retval

def chmod(path, mode):
    retval = lib.fs_chmod(path, c_int(mode))
    return retval

def utime(path, actime, modtime):
    retval = lib.fs_utime(path, c_int(actime), c_int(modtime))
    return retval

def read(path, len, offset):
    buf = (c_char * len)()
    val = lib.fs_read(xbytes(path), buf, c_int(len), c_int(offset), byref(null_fi))
    if val < 0:
        return val,''
    return val, buf[0:val]

def write(path, data, offset):
    nbytes = len(data)
    val = lib.fs_write(path, xbytes(data), c_int(nbytes),
                               c_int(offset), byref(null_fi))
    return val

S_IFMT  = 0o0170000  # bit mask for the file type bit field
S_IFREG = 0o0100000  # regular file
S_IFDIR = 0o0040000  # directory

S_IFBLK = 0o0060000
S_IFCHR = 0o0020000
S_IFIFO = 0o0010000
S_IFLNK = 0o0120000

EPERM = 1            # Error codes
ENOENT = 2           # ...
EIO = 5
ENOMEM = 12
ENOTDIR = 20
EISDIR = 21
EINVAL = 22
ENOSPC = 28
EOPNOTSUPP = 95
EEXIST = 17
ENOTEMPTY = 39

errors = { EPERM : 'EPERM', ENOENT : 'ENOENT', EIO : 'EIO',
               ENOMEM : 'ENOMEM', ENOTDIR : 'ENOTDIR',
               EISDIR : 'EISDIR', EINVAL : 'EINVAL',
               ENOSPC : 'ENOSPC', EOPNOTSUPP : 'EOPNOTSUPP',
               EEXIST : 'EEXIST', ENOTEMPTY : 'ENOTEMPTY'}

def strerr(err):
    if err < 0:
        err = -err
    else:
        return "OK"
    if err in errors:
        return errors[err]
    return 'UNKNOWN (%d)' % err
