#pragma once
#include <cstddef>
constexpr int SHUT_RDWR=2;
int shutdown(int fd, int mode);
int send(int fd, const void *data, size_t length, int flags);
