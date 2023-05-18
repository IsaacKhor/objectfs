#include <shared_mutex>
#include <variant>

#include "utils.hpp"

int main()
{
    auto t = "/";
    for (auto a : split_string_on_char(t, '/')) {
        std::cout << a.length();
        std::cout << a << std::endl;
    }
}
