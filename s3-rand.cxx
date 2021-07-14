#include <stdlib.h>
#include <mutex>
#include <condition_variable>
#include <list>
#include <libs3.h>
#include "s3wrap.h"
#include <sys/uio.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <unistd.h>


// maybe consider using https://github.com/p-ranav/argparse

long n_complete;
std::mutex m;
bool stop;

void read_thread(s3_target *tgt, std::string key, ssize_t size)
{
    ssize_t npages = size / 4096;
    char buf[4096];
    struct iovec iov[] = {{.iov_base = (void*)buf, .iov_len = sizeof(buf)}};

    while (!stop) {
	long pg = random() % npages;
	ssize_t offset = pg * 4096;
	tgt->s3_get(key, offset, 4096, iov, 1);
	m.lock();
	n_complete++;
	m.unlock();
    }
}


// s3-rand bucket/key nthreads seconds

int main(int argc, char **argv)
{
    char *host = getenv("S3_HOSTNAME");
    char *access = getenv("S3_ACCESS_KEY_ID");
    char *secret = getenv("S3_SECRET_ACCESS_KEY");

    char *bucket, *key;
    sscanf(argv[1], "%m[^/]/%ms", &bucket, &key);
    std::string s_key(key);

    int nthreads = atoi(argv[2]);
    int n_secs = atoi(argv[3]);

    auto tt = s3_target(host, bucket, access, secret, false);

    ssize_t obj_size;
    if (argc > 4)
	obj_size = atol(argv[4]);
    else if (tt.s3_head(s_key, &obj_size) != S3StatusOK) {
	printf("error\n");
	exit(1);
    }
    std::thread th[nthreads];
    std::cout << "bytes: " << obj_size << "\n";
    
    for (int i = 0; i < nthreads; i++) {
	auto t = new s3_target(host, bucket, access, secret, false);
	th[i] = std::thread(read_thread, t, s_key, obj_size);
    }

    auto start = std::chrono::system_clock::now();
    long to_date = 0;

//    std::cout << "now<end: " << (now<end) << "\n";
    for (int i = 0; i < n_secs; i++) {
	usleep(1000000);
	long tmp = n_complete;
	printf("%ld\n", tmp - to_date);
	to_date = tmp;
    }

    stop = true;
    for (int i = 0; i < nthreads; i++)
	th[i].join();

    auto end = std::chrono::system_clock::now();
    
    std::chrono::duration<double> t = end-start;
    
    printf("%ld in %.2f: %.2f/sec\n", n_complete, t.count(), 1.0 * n_complete / t.count());

    return 0;
}
