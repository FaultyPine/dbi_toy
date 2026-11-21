#ifndef PEONY_SHARED_DEFINES_H
#define PEONY_SHARED_DEFINES_H

#include <stdbool.h>
#include <stdint.h>
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#define STRINGIFY(x) #x
#define STRINGIFY_MACRO(x) STRINGIFY(x)

#define KB (1024ULL)
#define MB (KB * 1024ULL)
#define GB (MB * 1024ULL)
#define TB (GB * 1024ULL)

#endif