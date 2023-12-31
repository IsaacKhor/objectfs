#!/usr/bin/python3
from re import T
import unittest
import objtest as obj
import os, sys
import ctypes
import shutil
import time
import threading
import multiprocessing
from atomic import AtomicLong

prefix = 'prefix1'

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

def mismatch(a, b, n):
    n += 1
    while n < len(a) and n < len(b) and a[n] != b[n]:
        n += 1
    return n


class tests(unittest.TestCase):
    t_id = 0
    
    lk = threading.Lock()

    def assertOK(self, code, _msg):
        self.assertTrue(code >= 0, msg=_msg + ' : ' + obj.strerr(code))

    def run_mkdir(self, dirs, tid):
        print("run id: " + str(tid))
        for path,mode in dirs:
            v = obj.mkdir(path, mode)
            self.assertOK(v, 'mkdir %s %o' % (path, mode))
            v,sb = obj.getattr(path)
            self.assertOK(v, 'getattr %s' % path)
            self.assertEqual(sb.st_mode, (obj.S_IFDIR | mode),
                                    msg=('getattr %s: mode %o (should be %o)' %
                                            (path, sb.st_mode, (obj.S_IFDIR | mode))))
            v,des = obj.readdir(path)
            self.assertOK(v, 'readdir %s' % path)
            self.assertEqual(len(des), 0,
                                    msg='readdir %s: %d results (should be 0)' % (path, len(des)))
        print("quit id: " + str(tid))
    
    '''def test_01_mkdir(self):
        print('Test 1, mkdir (top level):')

        obj.init(prefix)
        obj.mkdir("/test_1", 0o755)

        dirs_1 = (('/test_1/dir' + str(s), 0o755) for s in range(10000))
        dirs_2 = (('/test_1/dir' + str(s), 0o700) for s in range(10000, 20000))
        dirs_3 = (('/test_1/dir' + str(s), 0o777) for s in range(20000, 30000))

        jobs = []
        
        tid = 0
        for dirs in [dirs_1, dirs_2, dirs_3]:
            jobs.append(threading.Thread(target=self.run_mkdir, args=(dirs, tid, )))
            tid += 1

        for j in jobs:
            j.start()

        print("JOIN: ")
        for j in jobs:
            j.join()
            
        print("BEFORE SYNC!\n")
        obj.sync()
        print("SYNC!\n")
        obj.teardown()
        print("TEARDOWN!\n")
        obj.init(prefix)
        print("AFTER INIT\n")
        
        for dirs in [dirs_1, dirs_2, dirs_3]:
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
        obj.teardown()'''

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
        v,data2 = obj.read(path, len(data),0)
        self.assertOK(v, 'read %s len=%d' % (path, len(data)))
        if data != data2:
            i = longest(data,data2)
            i2 = mismatch(data, data2, i)
            print('DATA len=%d, DATA2 len=%d, missmatch at %d UNTIL %d' % (len(data), len(data2), i, i2))
            print('mismatch: data="', data[i:i+8], '" data2="', data2[i:i+8], '"')

        self.assertTrue(data2 == data, msg='path=%s len(data2)=%d,len(data)=%d' % (path, len(data2),len(data)))

    def run_write(self, path, filesz, opsz, t_id):
        print("START WRITE TID: %d" % t_id)
        self.do_write(path, filesz, opsz)
        print("CHECK WRITE TID: %d" % t_id)
        self.check_write(path, filesz, opsz)
        print("JOIN WRITE TID: %d" % t_id)

    def test_06_write(self):
        print('Test 6, write')
        obj.init(prefix)
        obj.lib.test_function(ctypes.c_int(0))

        topdir = '/test_6'
        v = obj.mkdir(topdir, 0o777)
        self.assertOK(v, 'mkdir %s' % topdir)

        filesizes = (12, 577, 1011, 2001, 8099, 37000, 289150)
        opsizes = (17, 500, 3000)

        jobs = []
        t_id = 0
        for n in filesizes:
            #print('file size ', n)
            dir = 'dir-%d' % n
            dd = topdir + '/' + dir
            v = obj.mkdir(dd, 0o777)
            self.assertOK(v, 'mkdir %s' % dd)
            for m in opsizes:
                #print('op size ', m)
                path = dd + '/' + ('file-%d' % m)
                jobs.append(threading.Thread(target=self.run_write, args=(path, n, m, t_id, )))
                t_id += 1

        for j in jobs:
            j.start()

        print("JOIN: ")
        for j in jobs:
            j.join()

        print("BEFORE SYNC!\n")
        obj.sync()
        print("SYNC!\n")
        obj.teardown()
        print("TEARDOWN!\n")
        time.sleep(5)
        
        obj.init(prefix)
        print("INIT!")
        
        for n in filesizes:
            dir = 'dir-%d' % n
            dd = topdir + '/' + dir
            print("check "+dir)
            for m in opsizes:
                path = dd + '/' + ('file-%d' % m)
                self.check_write(path, n, m)
        print("BEFORE SECOND TEARDOWN")
        obj.teardown()
        print("SECOND TEARDOWN")

if __name__ == '__main__':
    os.system("python3 minio_cli.py")
    dir = "/local0/mount1"
    try:
        for de in os.scandir(dir):
            if os.path.isfile(dir + '/' + de.name):
                print('deleting file: ', dir+'/'+de.name)
                val = os.unlink(dir + '/' + de.name) 
            else:
                print('deleting dir: ', dir+'/'+de.name)
                shutil.rmtree(dir + '/' + de.name)
    except OSError(e):
        pass

    obj.set_context("songs", "minio", "miniostorage", "10.255.23.109:9000", 262144, 2000)
    #obj.init(prefix)
    unittest.main()
    
        