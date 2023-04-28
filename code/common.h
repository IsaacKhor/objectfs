#pragma once
#include <stdio.h>

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
