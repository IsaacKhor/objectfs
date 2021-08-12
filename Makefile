
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC -Is3lib/inc
CFLAGS = -g -Wall 

all: objfs-mount libobjfs.o libobjfs.so

objfs-mount : objfs-mount.o objfs.o s3wrap.o iov.o
	g++ -g $^ -o $@ -lfuse -ls3 -lcurl -lcrypto -lxml2 -Llibs3/build/lib -L/lib/x86_64-linux-gnu

libobjfs.o : CFLAGS += -shared -fPIC

libobjfs.so: objfs.o libobjfs.o 
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ 

clean:
	rm -f *.o *.so

