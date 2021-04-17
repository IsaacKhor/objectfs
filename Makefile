
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC
CFLAGS = -g -Wall -shared -fPIC -Wno-deprecated-declarations

# " -fPIC -shared "
libobjfs.so: objfs.o libobjfs.o 
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ 

clean:
	rm -f *.o *.so

