CC = clang
CXX = clang++
CXXFLAGS = -Wall -Wextra -Wconversion
CXXFLAGS += -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
CXXFLAGS += -std=c++2b -g -fPIC -I../libs3/inc


objectfs.o: objectfs.cpp objectfs.hpp
	$(CXX) -c objectfs.cpp $(CXXFLAGS) -o objectfs.o

models.o: models.cpp models.hpp
	$(CXX) -c models.cpp $(CXXFLAGS) -o models.o

fuse_interface.o: fuse_interface.cpp fuse_interface.hpp
	$(CXX) -c fuse_interface.cpp $(CXXFLAGS) -o fuse_interface.o

