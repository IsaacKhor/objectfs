
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC
CFLAGS = -g -Wall -shared -fPIC 

libobjfs.so: objfs.o libobjfs.o
	gcc -g $^ -o $@ -g -Wall -shared -fPIC

clean:
	rm -f *.o *.so

