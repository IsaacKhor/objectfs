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
    _fields_ = [("name", c_char * 256),	  # 0
                ("st_dev", c_longlong),	  # 64
                ("st_ino", c_longlong),	  # 72
                ("st_nlink", c_longlong), # 80
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

null_fi = fuse_file_info()
null_fi.fh = c_longlong(0)

dir = os.getcwd()
lib = CDLL(dir + "/libobjfs.so")
assert lib

verbose = c_int.in_dll(lib, "verbose")

def xbytes(path):
    if sys.version_info > (3,0):
        return bytes(path, 'UTF-8')
    else:
        return bytes(path)

def segfaulted():
    return c_int.in_dll(lib,'segv_was_called')

def stacktrace():
    return 'segfault'
#return str(c_char_p.in_dll(lib,'segv_stack_trace').value, 'UTF-8')

def mkfs(pfx):
    return lib.py_mkfs(xbytes(pfx))

def init(pfx):
    return lib.py_init(xbytes(pfx))

def teardown():
    lib.fs_teardown()

def getattr(path):
    sb = stat()
    retval = lib.py_getattr(xbytes(path), byref(sb))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval, sb

dir_max = 128
def readdir(path):
    des = (dirent * dir_max)()
    n = c_int(dir_max)
    val = lib.py_readdir(xbytes(path), byref(n), byref(des), byref(null_fi))
    if segfaulted():
        raise AssertionError(stacktrace())
    if val >= 0:
        return val, des[0:n.value]
    else:
        return val, []

def create(path, mode):
    retval = lib.py_create(xbytes(path), c_int(mode), byref(null_fi))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def mkdir(path, mode):
    #print('mkdir path:', path)
    retval = lib.py_mkdir(xbytes(path), c_int(mode))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def truncate(path, offset):
    retval = lib.py_truncate(xbytes(path), c_int(offset))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def unlink(path):
    retval = lib.py_unlink(xbytes(path))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def rmdir(path):
    retval = lib.py_rmdir(xbytes(path))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def rename(path1, path2):
    retval = lib.py_rename(xbytes(path1), xbytes(path2))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def chmod(path, mode):
    retval = lib.py_chmod(xbytes(path), c_int(mode))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def utime(path, actime, modtime):
    retval = lib.py_utime(xbytes(path), c_int(actime), c_int(modtime))
    if segfaulted():
        raise AssertionError(stacktrace())
    return retval

def read(path, len, offset):
    buf = (c_char * len)()
    val = lib.py_read(xbytes(path), buf, c_int(len), c_int(offset), byref(null_fi))
    if segfaulted():
        raise AssertionError(stacktrace())
    if val < 0:
        return val,''
    return val, buf[0:val]

def write(path, data, offset):
    nbytes = len(data)
    val = lib.py_write(xbytes(path), data, c_int(nbytes),
                               c_int(offset), byref(null_fi))
    if segfaulted():
        raise AssertionError(stacktrace())
    return val

def set_context(bucket, access_key, secret_key, host, size):
    lib.set_objectfs_context(xbytes(bucket), xbytes(access_key), xbytes(secret_key), xbytes(host), c_int(size))

def sync():
    lib.py_sync()
    
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
