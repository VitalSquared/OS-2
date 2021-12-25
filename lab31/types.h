#include "cache.h"
#include "picohttpparser.h"

#ifndef LAB31_TYPES_H
#define LAB31_TYPES_H

typedef struct http {
    int sock_fd, code, clients, status, is_response_complete;
    int response_type, headers_size; ssize_t response_size; ssize_t response_alloc_size;
    struct phr_chunked_decoder decoder;
    char *data;     ssize_t data_size;
    char *request; ssize_t request_size; ssize_t request_bytes_written;
    char *host, *path;
    cache_entry_t *cache_entry;
    struct http *prev, *next;
} http_t;

typedef struct client {
    int sock_fd, status;
    cache_entry_t *cache_entry;  http_t *http_entry;
    char *request;  ssize_t request_size; ssize_t request_alloc_size;
    ssize_t bytes_written;
    struct client *prev, *next;
} client_t;

typedef struct http_list {
    http_t *head;
} http_list_t;

typedef struct client_list {
    client_t *head;
} client_list_t;

#endif
