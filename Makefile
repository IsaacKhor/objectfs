
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC
CFLAGS = -g -Wall 

objfs-mount : objfs-mount.o objfs.o
	gcc -g $^ -o $@ -lstdc++ -lfuse

libobjfs.o : CFLAGS += -shared -fPIC

libobjfs.so: objfs.o libobjfs.o 
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ 

clean:
	rm -f *.o *.so

