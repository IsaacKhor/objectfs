CC = clang
CXX = g++-12
CXXFLAGS = -Wall -Wextra -Wconversion
CXXFLAGS += -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
CXXFLAGS += -std=c++2b -g -fPIC -I../libs3/inc
CXXLIBS = -lfuse -L../libs3/lib -ls3 -lfmt

all: objectfs

backend.o: backend.cpp backend.hpp
	$(CXX) -c backend.cpp $(CXXFLAGS) -o backend.o

objectfs.o: objectfs.cpp objectfs.hpp
	$(CXX) -c objectfs.cpp $(CXXFLAGS) -o objectfs.o

models.o: models.cpp models.hpp
	$(CXX) -c models.cpp $(CXXFLAGS) -o models.o

s3wrap.o: s3wrap.cpp s3wrap.hpp
	$(CXX) -c s3wrap.cpp $(CXXFLAGS) -o s3wrap.o

fuse_interface.o: fuse_interface.cpp 
	$(CXX) -c fuse_interface.cpp $(CXXFLAGS) -o fuse_interface.o

objectfs: backend.o objectfs.o models.o s3wrap.o fuse_interface.o
	$(CXX) backend.o objectfs.o models.o s3wrap.o fuse_interface.o -o objectfs $(CXXFLAGS) $(CXXLIBS)

.PHONY: clean

clean:
	rm -f *.o objectfs
