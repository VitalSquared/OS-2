#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include "http.h"
#include "states.h"
#include "list.h"

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path, http_list_t *http_list) {
    http_t *new_http = (http_t *)calloc(1, sizeof(http_t));
    if (new_http == NULL) {
        if (ERROR_LOG) perror("create_http: Unable to allocate memory for http struct");
        return NULL;
    }
    if (http_init(new_http, sock_fd, request, request_size, host, path) == -1) {
        free(new_http);
        return NULL;
    }
    http_add_to_list(new_http, http_list);
    if (INFO_LOG) printf("[%s %s] Connected\n", host, path);
    return new_http;
}

void remove_http(http_t *http, http_list_t *http_list, cache_t *cache) {
    http_remove_from_list(http, http_list);
    if (INFO_LOG) printf("[%d %s %s] Disconnected\n", http->sock_fd, http->host, http->path);
    http_destroy(http, cache);
    free(http);
}

int http_init(http_t *http, int sock_fd, char *request, ssize_t request_size, char *host, char *path) {
    http->status = AWAITING_REQUEST;
    http->clients = 1;  //we create http if there is a request, so we already have 1 client
    http->data = NULL; http->data_size = 0;
    http->code = HTTP_CODE_UNDEFINED;
    http->headers_size = HTTP_NO_HEADERS;
    http->response_type = HTTP_RESPONSE_NONE;
    http->is_response_complete = FALSE;
    http->decoder.consume_trailer = 1;
    http->sock_fd = sock_fd;
    http->request = request;
    http->request_size = request_size;
    http->response_alloc_size = 0;
    http->request_bytes_written = 0;
    http->host = host; http->path = path;
    http->cache_entry = NULL;
    return 0;
}

void http_destroy(http_t *http, cache_t *cache) {
    if (http->cache_entry != NULL && !http->cache_entry->is_full) {
        cache_remove(http->cache_entry, cache);
        http->cache_entry = NULL;
    }
    else if (http->cache_entry == NULL) {
        free(http->data);
        free(http->host);
        free(http->path);
    }
    close_socket(&http->sock_fd);
}

int http_check_disconnect(http_t *http) {
    if (http->clients == 0) {
        if (IS_ERROR_OR_DONE_STATUS(http->status)) {
            return TRUE;
        }
        #ifdef DROP_HTTP_NO_CLIENTS
        close_socket(&http->sock_fd);
        return TRUE;
        #endif
    }
    return FALSE;
}

const char *get_host_error(int err_code) {
    const char *err_msg;
    switch (err_code) {
        case HOST_NOT_FOUND: err_msg = "Authoritative Answer, Host not found"; break;
        case TRY_AGAIN: err_msg = "Non-Authoritative, Host not found, or SERVERFAIL"; break;
        case NO_RECOVERY: err_msg = "Non recoverable errors, FORMERR, REFUSED, NOTIMP"; break;
        case NO_DATA: err_msg = "Valid name, no data record of requested type"; break;
        default: err_msg = "Unknown error"; break;
    }
    return err_msg;
}

int http_open_socket(const char *hostname, int port) {
    int err_code;
    struct hostent *server_host = getipnodebyname(hostname, AF_INET, 0, &err_code);
    if (server_host == NULL) {
        if (ERROR_LOG) fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(err_code));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server_host->h_addr_list[0], sizeof(struct in_addr));

    freehostent(server_host);

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        if (ERROR_LOG) perror("open_http_socket: socket error");
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
        if (ERROR_LOG) perror("open_http_socket: connect error");
        close(sock_fd);
        return -1;
    }

    if (fcntl(sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("open_http_socket: fcntl error");
    }

    return sock_fd;
}

void http_goes_error(http_t *http) {
    http->status = SOCK_ERROR;
    close_socket(&http->sock_fd);
    http->data_size = 0;
    http->is_response_complete = FALSE;
}

void parse_http_response_headers(http_t *http) {
    int minor_version, status;
    const char *msg;
    size_t msg_len;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int headers_size = phr_parse_response(http->data, http->data_size, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);
    if (headers_size == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_http_response: Unable to parse http response headers\n");
        http_goes_error(http);
        return;
    }

    if (status != 0) http->code = status; //request may be incomplete, but it managed to get status
    if (headers_size >= 0) http->headers_size = headers_size;

    http->response_type = HTTP_RESPONSE_NONE;
    for (int i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Transfer-Encoding", strlen("Transfer-Encoding")) &&
        strings_equal_by_length(headers[i].value, headers[i].value_len, "chunked", strlen("chunked"))) {
            http->response_type = HTTP_RESPONSE_CHUNKED;
        }
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Content-Length", strlen("Content-Length"))) {
            http->response_type = HTTP_RESPONSE_CONTENT_LENGTH;
            http->response_size = get_number_from_string_by_length(headers[i].value, headers[i].value_len);
            if (http->response_size == -1) {
                http_goes_error(http);
                return;
            }
        }
    }
}

void parse_http_response_chunked(http_t *entry, char *buf, ssize_t offset, ssize_t size, cache_t *cache) {
    size_t rsize = size;
    ssize_t pret;

    pret = phr_decode_chunked(&entry->decoder, buf + offset, &rsize);
    if (pret == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_http_response_chunked: Unable to parse response\n");
        http_goes_error(entry);
        return;
    }

    if (entry->code == 200) {
        if (entry->cache_entry == NULL) {
            entry->cache_entry = cache_add(entry->host, entry->path, entry->data, entry->data_size, cache);
            if (entry->cache_entry == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
        }
    }

    if (pret == 0) {
        if (entry->cache_entry != NULL) entry->cache_entry->is_full = TRUE;
        entry->is_response_complete = TRUE;
    }
}

void parse_http_response_by_length(http_t *entry, cache_t *cache) {
    if (entry->code == 200) {
        if (entry->cache_entry == NULL) {
            entry->cache_entry = cache_add(entry->host, entry->path, entry->data, entry->data_size, cache);
            if (entry->cache_entry == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
        }
    }
    if (entry->data_size == entry->headers_size + entry->response_size) {
        if (entry->cache_entry != NULL) entry->cache_entry->is_full = TRUE;
        entry->is_response_complete = TRUE;
    }
}

void http_read_data(http_t *entry, cache_t *cache) {
    char buf[BUF_SIZE];
    errno = 0;
    ssize_t bytes_read = recv(entry->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("http_read_data: Unable to read from http socket");
        http_goes_error(entry);
        return;
    }
    if (bytes_read == 0) {
        entry->status = SOCK_DONE;
        if (entry->response_type == HTTP_RESPONSE_NONE) {
            entry->is_response_complete = TRUE;
            if (entry->cache_entry != NULL) entry->cache_entry->is_full = TRUE;
        }
        close_socket(&entry->sock_fd);
        return;
    }

    if (entry->status != DOWNLOADING) {
        if (ERROR_LOG) fprintf(stderr, "http_read_data: reading from http when we shouldn't\n");
        //if (INFO_LOG) write(STDERR_FILENO, buf, bytes_read);
        return;
    }

    if (entry->data_size + bytes_read > entry->response_alloc_size) {
        entry->response_alloc_size += BUF_SIZE;
        char *check = (char *)realloc(entry->data, entry->response_alloc_size);
        if (check == NULL) {
            if (ERROR_LOG) perror("http_read_data: Unable to reallocate memory for http data");
            http_goes_error(entry);
            return;
        }
        entry->data = check;
    }

    memcpy(entry->data + entry->data_size, buf, bytes_read);
    entry->data_size += bytes_read;

    int b_no_headers = entry->headers_size == HTTP_NO_HEADERS;
    if (entry->headers_size == HTTP_NO_HEADERS) parse_http_response_headers(entry);
    if (entry->status == SOCK_ERROR) return;

    if (entry->headers_size >= 0) {
        if (entry->response_type == HTTP_RESPONSE_CHUNKED) {
            parse_http_response_chunked(entry, buf, b_no_headers ? entry->headers_size : 0, b_no_headers ? entry->data_size - entry->headers_size : bytes_read, cache);
        }
        else if (entry->response_type == HTTP_RESPONSE_CONTENT_LENGTH) {
            parse_http_response_by_length(entry, cache);
        }
    }
}

void http_send_request(http_t *entry) {
    ssize_t bytes_written = write(entry->sock_fd, entry->request + entry->request_bytes_written, entry->request_size - entry->request_bytes_written);
    if (bytes_written >= 0) entry->request_bytes_written += bytes_written;
    if (entry->request_bytes_written == entry->request_size) {
        entry->status = DOWNLOADING;
        entry->request_size = 0;
        free_with_null((void **)&entry->request);
    }
    if (bytes_written == -1) {
        if (ERROR_LOG) perror("http_send_request: unable to write to http socket");
        http_goes_error(entry);
    }
}
