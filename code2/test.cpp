#include "utils.hpp"
#include <iostream>
#include <shared_mutex>
#include <variant>

struct Foo {
    int a;
    int b;
    int c;
    int pad;
    int pad2;
    // int pad3;
    int varl[];
};

int main()
{
    auto t = "/abc";
    auto split = split_string_on_char(t, '/');
    debug("split size: %d", split.size());
    for (auto &a : split) {
        std::cout << "len: " << a.length() << ", ";
        std::cout << "content:" << a << std::endl;
    }

    // std::cout << sizeof(Foo) << std::endl;
    // std::cout << offsetof(Foo, a) << std::endl;
    // std::cout << offsetof(Foo, b) << std::endl;
    // std::cout << offsetof(Foo, c) << std::endl;
    // std::cout << offsetof(Foo, pad) << std::endl;
    // std::cout << offsetof(Foo, pad2) << std::endl;
    // // std::cout << offsetof(Foo, pad3) << std::endl;
    // std::cout << offsetof(Foo, varl) << std::endl;
}
