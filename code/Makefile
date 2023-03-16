
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC -I../libs3/inc
CFLAGS = -g -Wall -I../libs3/inc

all: libobjfs.o libobjfs.so objfs-mount

CFLAGS += -shared -fPIC

iov.o: iov.c iov.h
	gcc -O -c iov.c -fPIC

libobjfs.so: s3wrap.o iov.o objfs.o libobjfs.o
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ -L../libs3/build/lib -l:libs3.so.4 -lfuse

objfs-mount: objfs-mount.o objfs.o s3wrap.o iov.o
	g++ -g $^ -o $@ -lfuse -l:libs3.a -lcurl -lcrypto -lxml2 -L../libs3/build/lib -L/lib/x86_64-linux-gnu -pthread

clean:
	rm -f *.o *.so
