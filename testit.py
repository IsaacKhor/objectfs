import unittest
import objtest as obj
import os, sys

prefix = '/tmp/testing/image'

def div_round_up(n, m):
    return (n + m - 1) // m

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

    def check_dirs(self, top, dirs):
        pass


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
    unittest.main()
            
