#include "cache.h"
#include "types.h"
#include "picohttpparser.h"

#ifndef LAB32_HTTP_H
#define LAB32_HTTP_H

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path, http_list_t *http_list, void *(*thread_func)(void *));
void remove_http(http_t *http, http_list_t *http_list, cache_t *cache);

int http_init(http_t *http, int sock_fd, char *request, ssize_t request_size, char *host, char *path);
void http_destroy(http_t *http, cache_t *cache);

int http_check_disconnect(http_t *http);
int http_open_socket(const char *hostname, int port);

void http_read_data(http_t *entry, cache_t *cache);
void http_send_request(http_t *entry);

#endif
