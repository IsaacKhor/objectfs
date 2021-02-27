
CXXFLAGS = -std=c++17 -g -Wall -shared -fPIC
CFLAGS = -g -Wall -shared -fPIC 

# " -fPIC -shared "
libobjfs.so: objfs.o libobjfs.o
	gcc -g $^ -o $@ -g -Wall -shared -fPIC -lstdc++ -Wl,--gc-sections

clean:
	rm -f *.o *.so

