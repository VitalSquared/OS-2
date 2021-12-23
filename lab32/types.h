#include "cache.h"
#include "picohttpparser.h"

#ifndef LAB32_TYPES_H
#define LAB32_TYPES_H

typedef struct http {
    int sock_fd, code, clients, status, is_response_complete, dont_accept_clients;
    int response_type, headers_size; ssize_t response_size;
    struct phr_chunked_decoder decoder;
    char *data;     ssize_t data_size;
    char *request;  ssize_t request_size;   ssize_t request_bytes_written;
    char *host, *path;
    cache_entry_t *cache_entry;
    pthread_t thread_id;
    pthread_rwlock_t rwlock;
    int client_pipe_fd, http_pipe_fd;
    struct http *prev, *next;
} http_t;

typedef struct client {
    int sock_fd, status;
    cache_entry_t *cache_entry;  http_t *http_entry;
    char *request;  ssize_t request_size;
    ssize_t bytes_written;
    pthread_t thread_id;
    struct client *prev, *next;
} client_t;

typedef struct http_list {
    http_t *head;
    pthread_rwlock_t rwlock;
} http_list_t;

typedef struct client_list {
    client_t *head;
    pthread_rwlock_t rwlock;
} client_list_t;

#endif
