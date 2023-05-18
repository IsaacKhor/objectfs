#pragma once
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>

#define ENABLE_DEBUG 1

#define debug(MSG, ...)                                                        \
    do {                                                                       \
        if (ENABLE_DEBUG)                                                      \
            fprintf(stderr, "[DBG (%s:%d %s)] " MSG "\n", __FILE__, __LINE__,  \
                    __func__, ##__VA_ARGS__);                                  \
    } while (0)

#define log_error(MSG, ...)                                                    \
    do {                                                                       \
        if (ENABLE_DEBUG)                                                      \
            fprintf(stderr, "[ERR (%s:%d %s)] " MSG "\n", __FILE__, __LINE__,  \
                    __func__, ##__VA_ARGS__);                                  \
    } while (0)

inline std::vector<std::string> split_string_on_char(const std::string &s,
                                                     char delim)
{
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;

    while (getline(ss, item, delim)) {
        result.push_back(item);
    }

    return result;
}

inline std::string string_join(const std::vector<std::string> &strings,
                               const std::string &delim)
{
    std::string result;
    for (auto it = strings.begin(); it != strings.end(); it++) {
        result += *it;
        if (it != strings.end() - 1)
            result += delim;
    }
    return result;
}

/**
 * Python style array slicing. Copied from
 * https://stackoverflow.com/questions/50549611/
 */
template <int left = 0, int right = 0, typename T>
inline constexpr auto slice(T &&container)
{
    if constexpr (right > 0) {
        return std::span(begin(std::forward<T>(container)) + left,
                         begin(std::forward<T>(container)) + right);
    } else {
        return std::span(begin(std::forward<T>(container)) + left,
                         end(std::forward<T>(container)) + right);
    }
}
