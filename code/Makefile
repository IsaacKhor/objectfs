CC = clang
CXX = clang++
CXXFLAGS = -std=c++20 -g -Wall -fPIC -I../libs3/inc
CFLAGS = -g -Wall -I../libs3/inc

all: libobjfs.o libobjfs.so objfs-mount

CFLAGS += -fPIC

iov.o: iov.c iov.h
	$(CC) -O -c iov.c -fPIC

objects.o: objects.cc objects.h
	$(CC) -g -c objects.cc $(CXXFLAGS) -o objects.o

libobjfs.so: s3wrap.o iov.o objfs.o libobjfs.o objects.o
	$(CC) -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ -L../libs3/build/lib -l:libs3.so.4 -lfuse -lfmt

objfs-mount: objfs-mount.o objfs.o s3wrap.o iov.o objects.o
	$(CXX) -g $^ -o $@ -lfuse -l:libs3.a -lcurl -lcrypto -lxml2 -L../libs3/build/lib -L/lib/x86_64-linux-gnu -pthread -lfmt

clean:
	rm -f *.o *.so

