#pragma once
#include <obs-module.h>
#define PLOG "[multisite] "
#define mlog_info(f, ...)  blog(LOG_INFO,    PLOG f, ##__VA_ARGS__)
#define mlog_warn(f, ...)  blog(LOG_WARNING, PLOG f, ##__VA_ARGS__)
#define mlog_error(f, ...) blog(LOG_ERROR,   PLOG f, ##__VA_ARGS__)
