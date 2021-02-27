#include <stdio.h>
#include <queue>
#include <map>
#include <memory>

class openfile {
public:
    int index;
    int fd;
};

/*
std::map<int,openfile*> file_cache;
std::queue<openfile*> file_fifo;
*/

std::map<int,std::shared_ptr<openfile>> file_cache;
std::queue<std::shared_ptr<openfile>> file_fifo;
int next_fd;

int get_fd(int index)
{
    int fd;
    
    if (file_cache.find(index) != file_cache.end()) {
	auto of = file_cache[index];
	printf("cache hit: %d -> %d\n", index, of->fd);
	fd = of->fd;
    }
    else {
	printf("miss:\n");
	if (file_fifo.size() >= 10) {
	    auto of = file_fifo.front();
	    printf("  evicting %d -> %d\n", of->index, of->fd);
	    file_cache.erase(of->index);
	    file_fifo.pop();
	}
	auto of = std::make_shared<openfile>();
	printf(" inserting %d -> %d\n", index, next_fd);
	of->index = index;
	of->fd = next_fd;
	file_cache[index] = of;
	file_fifo.push(of);

	fd = next_fd++;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int n = atoi(argv[1]);

    for (int i = 0; i < n; i++) {
#if 0
	openfile *of = new openfile;
	of->index = 0;
	delete of;
#endif
	int x;
	if (rand() % 100 < 50)
	    x = rand() % 5;
	else
	    x = rand() % 25;
	get_fd(x);
    }
}
