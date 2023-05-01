CC = clang
CXX = clang++
CXXFLAGS = -std=c++20 -g -Wall -shared -fPIC -I../libs3/inc
CFLAGS = -g -Wall -I../libs3/inc

all: libobjfs.o libobjfs.so objfs-mount

CFLAGS += -shared -fPIC

iov.o: iov.c iov.h
	$(CC) -O -c iov.c -fPIC

libobjfs.so: s3wrap.o iov.o objfs.o libobjfs.o
	$(CC) -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ -L../libs3/build/lib -l:libs3.so.4 -lfuse -lfmt

objfs-mount: objfs-mount.o objfs.o s3wrap.o iov.o
	$(CXX) -g $^ -o $@ -lfuse -l:libs3.a -lcurl -lcrypto -lxml2 -L../libs3/build/lib -L/lib/x86_64-linux-gnu -pthread -lfmt

clean:
	rm -f *.o *.so

