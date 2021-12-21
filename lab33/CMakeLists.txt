cmake_minimum_required(VERSION 3.9)
project(PROXY-1 C)

set(CMAKE_C_STANDARD 99)

add_executable(proxy proxy.c picohttpparser.h picohttpparser.c cache.h cache.c http.h http.c client.h client.c states.h states.c list_queue.h list_queue.c types.h)
