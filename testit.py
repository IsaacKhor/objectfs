import unittest
import objtest as obj
import os, sys
import ctypes

prefix = '/tmp/testing/image'

def div_round_up(n, m):
    return (n + m - 1) // m

def longest(a, b):
    L = min(len(a),len(b))
    n = L//2
    i = n
    while i > 0:
        if a[0:n] != b[0:n]:
            n = n - i
        else:
            n = n + i
        i = i // 2
    while a[0:n] == b[0:n]:
        n += 1
    while a[0:n] != b[0:n]:
        n -= 1
    return n

class tests(unittest.TestCase):

    def assertOK(self, code, _msg):
        self.assertTrue(code >= 0, msg=_msg + ' : ' + obj.strerr(code))
    
    def test_01_mkdir(self):
        print('Test 1, mkdir (top level):')

        dirs = (('/test1', 0o755), ('/test2', 0o700), ('/test3', 0o777))
        for path,mode in dirs:
            v = obj.mkdir(path, mode)
            self.assertOK(v, 'mkdir %s %o' % (path, mode))

        for path,mode in dirs:
            v,sb = obj.getattr(path)
            self.assertOK(v, 'getattr %s' % path)
            self.assertEqual(sb.st_mode, (obj.S_IFDIR | mode),
                                 msg=('getattr %s: mode %o (should be %o)' %
                                          (path, sb.st_mode, (obj.S_IFDIR | mode))))
            v,des = obj.readdir(path)
            self.assertOK(v, 'readdir %s' % path)
            self.assertEqual(len(des), 0,
                                 msg='readdir %s: %d results (should be 0)' % (path, len(des)))
        obj.sync()
        obj.teardown()
        obj.init(prefix)
        
        for path,mode in dirs:
            v,sb = obj.getattr(path)
            self.assertOK(v, 'getattr %s' % path)
            self.assertEqual(sb.st_mode, (obj.S_IFDIR | mode),
                                 msg=('getattr %s: mode %o (should be %o)' %
                                          (path, sb.st_mode, (obj.S_IFDIR | mode))))
            v,des = obj.readdir(path)
            self.assertOK(v, 'readdir %s' % path)
            self.assertEqual(len(des), 0,
                                 msg='readdir %s: %d results (should be 0)' % (path, len(des)))
        # FSCK???

    def check_dirs(self, mkdir, top, dirs):
        for d in dirs:
            path = top + '/' + d
            if mkdir:
                v = obj.mkdir(path, 0o777)
                self.assertOK(v,'check_dirs: mkdir %s' % path)
                v,sb = obj.getattr(path)
                self.assertOK(v, 'check_dirs %s: getattr' % path)
                self.assertEqual(sb.st_mode & obj.S_IFMT, obj.S_IFDIR,
                                     msg=('check_dirs: mode=%o (%o not set)' %
                                              (sb.st_mode,obj.S_IFDIR)))

        v,des = obj.readdir(top)
        self.assertOK(v, 'readdir %s' % top)
        names = set([str(de.name, 'UTF-8') for de in des])
        self.assertEqual(names, set(dirs), msg=('check_dirs: readdir: expected %s, got %s' %
                                                    (' '.join(names), ' '.join(dirs))))
        
    def test_02_mkdir(self):
        print('Test 2, mkdir (subdirectories)')

        # create new directories to make test standalone
        topdirs = ['/test2_%d' % i for i in range(3)]
        for path in topdirs:
            v = obj.mkdir(path, 0o777)
            self.assertOK(v, 'mkdir %s' % path)
        
        print('3 subdirs...')
        self.check_dirs(True, topdirs[0], ['dir%d' % i for i in range(3)])

        print('31 subdirs...')
        self.check_dirs(True, topdirs[1], ['dir%d' % i for i in range(31)])

        print('32 subdirs...')
        self.check_dirs(True, topdirs[2], ['dir%d' % i for i in range(32)])

        print('restarting...')
        obj.sync()
        obj.teardown()
        obj.init(prefix)

        print('3 subdirs...')
        self.check_dirs(False, topdirs[0], ['dir%d' % i for i in range(3)])

        print('31 subdirs...')
        self.check_dirs(False, topdirs[1], ['dir%d' % i for i in range(31)])

        print('32 subdirs...')
        self.check_dirs(False, topdirs[2], ['dir%d' % i for i in range(32)])

    def create_files(self, top, files, mode):
        for f in files:
            path = top + '/' + f
            v = obj.create(path, mode)
            self.assertOK(v, 'check_files: create %s' % path)
            v,sb = obj.getattr(path)
            self.assertOK(v, 'check_files: getattr %s' % path)
            self.assertEqual(sb.st_mode & obj.S_IFMT, obj.S_IFREG,
                                 msg='check_files: getattr: %o not set' % obj.S_IFREG)
            self.assertEqual(sb.st_size, 0, msg='check_files: st_size != 0')
            
    def check_files(self, top, files, mode):
        v,des = obj.readdir(top)
        self.assertOK(v, 'readdir %s' % top)
        names = set([str(de.name,'UTF-8') for de in des])
        self.assertEqual(names, set(files), msg=('readir: got "%s" (not "%s")' %
                                                     (' '.join(names), ' '.join(files))))
        for de in des:
            self.assertEqual(de.st_mode & 0o777, mode,
                                 msg=('readdir: %s: mode=%o (should be %o)' %
                                          (de.name, de.st_mode & 0o777, mode)))

    def test_03_create(self):
        print('Test 3, create')
        
        # create new directories to make test standalone
        topdirs = ['/test3_%d' % i for i in range(3)]
        for path in topdirs:
            v = obj.mkdir(path, 0o777)
            self.assertOK(v, 'mkdir %s' % path)

        print('3 files...')
        self.create_files(topdirs[0], ('file1', 'file2', 'file3'), 0o777)
        self.check_files(topdirs[0], ('file1', 'file2', 'file3'), 0o777)

        print('31 files...')
        self.create_files(topdirs[1], ['file%d' % i for i in range(31)], 0o744)
        self.check_files(topdirs[1], ['file%d' % i for i in range(31)], 0o744)

        print('32 files...')
        self.create_files(topdirs[2], ['file%d' % i for i in range(31)], 0o500)
        self.check_files(topdirs[2], ['file%d' % i for i in range(31)], 0o500)

        print('restarting...')
        obj.sync()
        obj.teardown()
        obj.init(prefix)
        
        print('3 files...')
        self.check_files(topdirs[0], ('file1', 'file2', 'file3'), 0o777)

        print('31 files...')
        self.check_files(topdirs[1], ['file%d' % i for i in range(31)], 0o744)

        print('32 files...')
        self.check_files(topdirs[2], ['file%d' % i for i in range(31)], 0o500)
        
    def test_04_create(self):
        print('Test 4, long names')

        topdir = '/test4'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        names = ('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', '00000000000000000000000000',
                     'CASEtest', 'casetest', 'CASETEST')
        for n in names:
            path = topdir + '/' + n
            v = obj.create(path, 0o777)
            self.assertOK(v, 'create %s' % path)
            
        longname = 'b' * 100
        path = topdir + '/' + longname
        v = obj.create(path, 0o777)
        self.assertOK(v, 'create %s' % path)

        v,sb = obj.getattr(path)
        self.assertOK(v, 'getattr %s' % path)
        
        v,des = obj.readdir(topdir)
        self.assertOK(v, 'readdir %s' % topdir)
        names = [str(de.name,'UTF-8') for de in des]
        trimmed_name = longname
        self.assertTrue(trimmed_name in names)

        print('restarting...')
        obj.sync()
        obj.teardown()
        obj.init(prefix)

        v,sb = obj.getattr(path)
        self.assertOK(v, 'getattr %s' % path)
        
        v,des = obj.readdir(topdir)
        self.assertOK(v, 'readdir %s' % topdir)
        names = [str(de.name,'UTF-8') for de in des]
        self.assertTrue(longname in names)

    def test_05_badpaths(self):
        print('Test 5, bad paths')

        topdir = '/test5'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        obj.create(topdir+'/file', 0o777)
        self.assertOK(v, 'mkdir %s' % (topdir+'/file'))
        
        v = obj.mkdir(topdir + '/dir', 0o777)
        self.assertOK(v, 'mkdir %s' % (topdir + '/dir'))

        tests = [('doesntexist/path', -obj.ENOENT),
                     ('file/path', -obj.ENOTDIR),
                     ('dir', -obj.EEXIST),
                     ('file', -obj.EEXIST)]

        print(' create:',end='')
        for path,err in tests:
            v = obj.create(topdir+'/'+path, 0o777)
            print(' ', path, ':', obj.strerr(v))
            self.assertEqual(v, err, msg="path=%s err=%d (not %d)" % (path,v, err))

        print('\n mkdir:',end='')
        for path,err in tests:
            print(' ', path, ':', obj.strerr(v))
            v = obj.mkdir(topdir+'/'+path, 0o777)
            self.assertEqual(v, err, msg="path=%s err=%d (not %d)" % (path,v, err))

    def do_write(self, path, filesz, opsz):
        v = obj.create(path, 0o777)
        self.assertOK(v, 'create %s' % path)

        print( path, filesz, opsz)
        data = b'1234567' * div_round_up(filesz, 7)
        data = data[0:filesz]

        for offset in range(0, filesz, opsz):
            nbytes = min(filesz-offset, opsz)
            chunk = data[offset:offset+nbytes]
            v = obj.write(path, chunk, offset)
            self.assertOK(v, 'write %s offset=%d len=%d' % (path, offset, len(chunk)))
            self.assertTrue(v, len(chunk))
        
    def check_write(self, path, filesz, opsz):
        data = b'1234567' * div_round_up(filesz, 7)
        data = data[0:filesz]
        if filesz == 37000 and opsz == 17:
            obj.lib.test_function(ctypes.c_int(0))
        v,data2 = obj.read(path, len(data)+100, 0)
        self.assertOK(v, 'read %s len=%d' % (path, len(data)+100))
        if data != data2:
            i = longest(data,data2)
            print('len=%d missmatch at %d' % (len(data), longest(data,data2)))
            print('mismatch: data="', data[i:i+8], '" data2="', data2[i:i+8], '"')

        self.assertTrue(data2 == data, msg='path=%s len(data2)=%d,len(data)=%d' % (path, len(data2),len(data)))

    
    def test_06_write(self):
        print('Test 6, write')

        topdir = '/test6'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        filesizes = (12, 577, 1011, 1024, 1025, 2001, 8099, 37000, 289150)
        opsizes = (17, 100, 1000, 1024, 1970, 3000)

        for n in filesizes:
            print('file size ', n)
            dir = 'dir-%d' % n
            dd = topdir + '/' + dir
            v = obj.mkdir(dd, 0o777)
            self.assertOK(v, 'mkdir %s' % dd)
            for m in opsizes:
                print('op size ', m)
                path = dd + '/' + ('file-%d' % m)
                self.do_write(path, n, m)
                self.check_write(path, n, m)

        v = obj.write(topdir + '/this_is_not_a_file', 'foooooooo', 0)
        self.assertEqual(-v, obj.ENOENT)

        obj.sync()
        obj.teardown()
        obj.init(prefix)
        
        for n in filesizes:
            dir = 'dir-%d' % n
            dd = topdir + '/' + dir
            for m in opsizes:
                path = dd + '/' + ('file-%d' % m)
                self.check_write(path, n, m)

    def test_07_unlink(self):
        print( 'Test 7, unlink')

        topdir = '/test7'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        filesizes = (12, 577, 1011, 1024, 1025, 2001, 8099, 37000, 289150)
        files = []
        for n in filesizes:
            path = topdir + '/' + ('file-%d' % n)
            files.append(path)
            v = obj.create(path, 0o777)
            self.assertOK(v, 'create %s' % path)

            data = 'a' * n
            v = obj.write(path, data, 0)
            self.assertOK(v, 'write %s, %d bytes' % (path, len(data)))

        for path in files:
            print( path)
            v = obj.unlink(path)
            self.assertOK(v, 'unlink %s' % path)
            v,sb = obj.getattr(path)
            self.assertEqual(-v, obj.ENOENT, msg='pre-sync: %s not deleted' % path)
            
        obj.sync()
        obj.teardown()
        obj.init(prefix)

        for path in files:
            v,sb = obj.getattr(path)
            self.assertEqual(-v, obj.ENOENT, msg='post-sync: %s not deleted' % path)
        
    def test_08_rmdir_unlink(self):
        print( 'Test 8, rmdir')

        topdir = '/test8'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        for d in ('dir1', 'dir2', 'dir3'):
            dir = topdir + '/' + d
            v = obj.mkdir(dir, 0o777)
            self.assertOK(v, 'mkdir(%s)' % dir)

        for d in ('a', 'b'):
            v = obj.mkdir(topdir + '/dir1/' + d, 0o777)
            self.assertOK(v, 'mkdir(%s)' % (topdir+'/dir1/'+d))

        for f in ('c', 'd'):
            file = topdir + '/dir2/' + f
            v = obj.create(file, 0o777)
            self.assertOK(v, 'create(%s)' % file)

        v = obj.rmdir(topdir + '/dir2/c')
        self.assertEqual(v, -obj.ENOTDIR, msg='rmdir(dir2/c) err=%d' % v)

        v = obj.rmdir(topdir + '/dir2')
        self.assertEqual(v, -obj.ENOTEMPTY, msg='rmdir(dir2) err=%d' % v)

        v = obj.rmdir(topdir + '/dir1')
        self.assertEqual(v, -obj.ENOTEMPTY, msg='rmdir(dir1) err=%d' % v)

        v = obj.unlink(topdir + '/dir3')
        self.assertEqual(v, -obj.EISDIR, msg='unlink(dir3) [directory] err=%d' % v)

        v = obj.unlink(topdir + '/dir1/a')
        self.assertEqual(v, -obj.EISDIR, msg='unlink(dir1/a) [directory] err=%d' % v)

        for d in ('/dir3', '/dir1/a', '/dir1/b'):
            v = obj.rmdir(topdir + d)
            self.assertOK(v, 'rmdir(%s)' % (topdir+d))

        for d in ('/dir3', '/dir1/a', '/dir1/b'):
            v,sb = obj.getattr(topdir + d)
            self.assertEqual(v, -obj.ENOENT, msg='pre-sync %s not removed' % (topdir+d))

        obj.sync()
        obj.teardown()
        obj.init(prefix)
            
        for d in ('/dir3', '/dir1/a', '/dir1/b'):
            v,sb = obj.getattr(topdir + d)
            self.assertEqual(v, -obj.ENOENT, msg='pre-sync %s not removed' % (topdir+d))
        
    def test_09_rename(self):
        print( 'Test 9, rename')

        topdir = '/test9'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        d1,d2,f1,f2 = [topdir + n for n in ('/dir1', '/dir2', '/file1', '/file2')]
        obj.mkdir(d1, 0o777)
        obj.create(f1, 0o777)

        v = obj.rename(d1, d2)
        self.assertOK(v, 'rename(%s, %s) [dirs]' % (d1,d2))
        v,sb = obj.getattr(d1)
        self.assertEqual(v, -obj.ENOENT, msg='rename: %s->%s' % (d1, d2))
        v,sb = obj.getattr(d2)
        self.assertOK(v, 'rename(%s, %s)' % (d1,d2))
        
        v = obj.rename(f1, f2)
        self.assertOK(v, 'rename(%s, %s) [files]' % (f1,f2))
        v,sb = obj.getattr(f1)
        self.assertEqual(v, -obj.ENOENT, msg='rename: %s->%s' % (f1, f2))
        v,sb = obj.getattr(f2)
        self.assertOK(v, 'rename(%s, %s)' % (f1,f2))

        v = obj.rename(d2, f2)
        self.assertEqual(-v, obj.EEXIST)

        v = obj.rename(f2, d2)
        self.assertEqual(-v, obj.EEXIST)

        v = obj.rename(f2, d2 + '/zz')
        self.assertTrue(v == 0 or -v == obj.EINVAL, msg='%s->%s = %d' % (f2,d2,v))

        v = obj.rename(d2 + 'bad', d1)
        self.assertTrue(-v == obj.ENOENT)

        obj.sync()
        obj.teardown()
        obj.init(prefix)

        v,sb = obj.getattr(d1)
        self.assertEqual(v, -obj.ENOENT, msg='post-sync rename: %s->%s' % (d1, d2))
        v,sb = obj.getattr(d2)
        self.assertOK(v, 'post-sync rename(%s, %s)' % (d1,d2))
        
        v,sb = obj.getattr(f1)
        self.assertEqual(v, -obj.ENOENT, msg='post-sync rename: %s->%s' % (f1, f2))
        v,sb = obj.getattr(f2)
        self.assertOK(v, 'post-sync rename(%s, %s)' % (f1,f2))

    def test_10_chmod(self):
        print( 'Test 10, chmod')
        topdir = '/test10'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        f = topdir + '/file'
        d = topdir + '/dir'
        obj.create(f, 0o777)
        obj.mkdir(d, 0o777)

        for m in (0o700, 0o666, 0o555, 0o133):
            v = obj.chmod(f, m)
            v,sb = obj.getattr(f)
            self.assertEqual(sb.st_mode & ~obj.S_IFMT, m)
            v = obj.chmod(d, m)
            v,sb = obj.getattr(d)
            self.assertEqual(sb.st_mode & ~obj.S_IFMT, m)

        v = obj.chmod(topdir + '/does-not-exist', 0o777)
        self.assertEqual(-v, obj.ENOENT)
        
        obj.sync()
        obj.teardown()
        obj.init(prefix)

        m = 0o133
        v,sb = obj.getattr(f)
        self.assertEqual(sb.st_mode & ~obj.S_IFMT, m)
        v = obj.chmod(d, m)
        v,sb = obj.getattr(d)
        self.assertEqual(sb.st_mode & ~obj.S_IFMT, m)
 
    def test_12_truncate(self):
        print( 'Test 12, truncate')
        topdir = '/test12'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        # basically re-uses the logic from unlink

        filesizes = (12, 577, 1011, 1024, 1025, 2001, 8099, 37000, 289150)
        files = []
        for n in filesizes:
            path = topdir + '/' + ('file-%d' % n)
            files.append(path)
            v = obj.create(path, 0o777)
            self.assertOK(v, 'create(%s)' % path)

            data = 'a' * n
            v = obj.write(path, data, 0)
            self.assertOK(v, 'write(%s, %d bytes)' % (path, len(data)))

        for path in files:
            print( path)
            v = obj.truncate(path, 0)
            self.assertOK(v, 'truncate(%s,0)' % path)
            v,sb = obj.getattr(path)
            self.assertOK(v, 'getattr(%s)' % path)
            self.assertEqual(sb.st_size, 0)

        v = obj.truncate(topdir + '/invalid-path', 0)
        self.assertEqual(-v, obj.ENOENT, msg='/invalid-path')
        
        obj.sync()
        obj.teardown()
        obj.init(prefix)

        for path in files:
            print( path)
            v,sb = obj.getattr(path)
            self.assertOK(v, 'getattr(%s)' % path)
            self.assertEqual(sb.st_size, 0)
        
if __name__ == '__main__':

    dir = os.path.dirname(prefix)
    try:
        for de in os.scandir(dir):
            print('deleting: ', dir+'/'+de.name)
            val = os.unlink(dir + '/' + de.name)
    except OSError(e):
        pass

    try:
        os.mkdir(dir)
    except OSError:
        pass
    
    fs = b'\x4f\x42\x46\x53\x01\x00\x00\x00\x01\x00\x00\x00\x3e\x00\x00\x00' + \
      b'\x00\x00\x00\x00\x81\x02\x01\x00\x00\x00\xe4\x41\x00\x00\x00\x00' + \
      b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x60\x94\xd1' + \
      b'\x3a\x60\x00\x00\x00\x00\x71\x47\xfa\x2e\x00\x00\x00\x00'
    fp = open(prefix + '.00000000', 'wb')
    fp.write(fs)
    fp.close()

    obj.init(prefix)
#    obj.lib.test_function(ctypes.c_int(1))
    unittest.main()
    
