#include "cache.h"
#include "picohttpparser.h"

#ifndef LAB33_TYPES_H
#define LAB33_TYPES_H

typedef struct http {
    int sock_fd, code, clients, status, error, is_response_complete, dont_accept_clients;
    int response_type, headers_size; ssize_t response_size;
    struct phr_chunked_decoder decoder;
    char *data;     ssize_t data_size;
    char *request;  ssize_t request_size;   ssize_t request_bytes_written;
    char *host, *path;
    cache_entry_t *cache_entry;
    pthread_rwlock_t rwlock;
    struct http *prev, *next;
    struct http *global_prev, *global_next;
} http_t;

typedef struct client {
    int sock_fd, status;
    cache_entry_t *cache_entry;  http_t *http_entry;
    char *request;  ssize_t request_size;
    ssize_t bytes_written;
    pthread_t thread_id;
    struct client *prev, *next;
    struct client *global_prev, *global_next;
} client_t;

typedef struct http_list {
    http_t *head;
    pthread_rwlock_t rwlock;
} http_list_t;

typedef struct client_list {
    client_t *head;
    pthread_rwlock_t rwlock;
} client_list_t;

typedef struct client_queue {
    client_t *head;
    client_t *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} client_queue_t;

typedef struct http_queue_t {
    http_t *head;
    http_t *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} http_queue_t;

#endif
