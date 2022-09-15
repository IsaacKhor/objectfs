
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC -Ilibs3/inc
CFLAGS = -g -Wall -Ilibs3/inc

all: libobjfs.o libobjfs.so objfs-mount

CFLAGS += -shared -fPIC

iov.o: iov.c iov.h
	gcc -O -c iov.c -fPIC

libobjfs.so: s3wrap.o iov.o objfs.o libobjfs.o
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ -ls3 -Llibs3/build/lib

objfs-mount: objfs-mount.o objfs.o s3wrap.o iov.o
	g++ -g $^ -o $@ -lfuse -ls3 -lcurl -lcrypto -lxml2 -Llibs3/build/lib -L/lib/x86_64-linux-gnu -pthread

clean:
	rm -f *.o *.so

