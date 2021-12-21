#include "cache.h"
#include "picohttpparser.h"

#ifndef LAB33_HTTP_H
#define LAB33_HTTP_H

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

int http_init(http_t *http, int sock_fd, char *request, ssize_t request_size, char *host, char *path);
void http_destroy(http_t *http);
int http_check_disconnect(http_t *http, cache_t *cache);
int http_open_socket(const char *hostname, int port, int *out_error);
void http_read_data(http_t *entry, cache_t *cache);
void http_send_request(http_t *entry);

#endif
