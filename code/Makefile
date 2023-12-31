CC = gcc
CXX = g++-12
CXXFLAGS = -g3 -std=c++2b -fPIC -I../libs3/inc
CXXLIBS = -lfuse -L../libs3/lib -ls3 -lfmt

mount_path = /tmp/obfs-test

debug: CXXFLAGS += -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment
debug: CXXFLAGS += -Wall -Wextra -Wconversion -Wdouble-promotion -Wno-sign-conversion
debug: CXXFLAGS += -DLOGLV=0
release: CXXFLAGS += -O3 -DLOGLV=2

debug: objectfs
release: objectfs

all: release

# $< is the first dependency
# $@ is the target file
# $^ is all dependencies
# $* is the target file without the extension (the % part)

%.o: %.cpp *.hpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

objectfs: backend.o objectfs.o models.o fuse_interface.o gc.o
	$(CXX) $^ -o $@ $(CXXFLAGS) $(CXXLIBS)

.PHONY: clean run-debug run-release

run-debug: export LSAN_OPTIONS=suppressions=./.asan_suppress
run-debug: export ASAN_OPTIONS=abort_on_error=1:halt_on_error=1
run-debug: export UBSAN_OPTIONS=abort_on_error=1:halt_on_error=1
run-debug: debug
	mkdir -p $(mount_path)
	rm -rf $(mount_path)/*
	./objectfs -f $(mount_path)

run-release: release
	mkdir -p $(mount_path)
	rm -rf $(mount_path)/*
	./objectfs -f $(mount_path)

clean:
	rm -f *.o objectfs
