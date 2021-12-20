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

int http_init(http_t *http, int sock_fd, char *request, ssize_t request_size, char *host, char *path) {
    int err_code = pthread_rwlock_init(&http->rwlock, NULL);
    if (err_code != 0) {
        if (ERROR_LOG) print_error("http_init: Unable to init rwlock", err_code);
        return -1;
    }

    http->status = AWAITING_REQUEST;
    http->clients = 1;  //we create http if there is a request, so we already have 1 client
    http->dont_accept_clients = FALSE;
    http->data = NULL; http->data_size = 0;
    http->code = HTTP_CODE_UNDEFINED;
    http->headers_size = HTTP_NO_HEADERS;
    http->is_response_complete = FALSE;
    http->decoder.consume_trailer = 1;
    http->sock_fd = sock_fd;
    http->request = request; http->request_size = request_size; http->request_bytes_written = 0;
    http->host = host; http->path = path;
    http->cache_entry = NULL;
    return 0;
}

void http_destroy(http_t *http) {
    if (http->cache_entry == NULL) {
        free(http->data);
        free(http->host);
        free(http->path);
    }
    pthread_rwlock_destroy(&http->rwlock);
}

int http_check_disconnect(http_t *http, cache_t *cache) {
    write_lock_rwlock(&http->rwlock, "http_check_disconnect: Unable to write-lock rwlock");
    if (http->clients == 0) {
        if (http->status == SOCK_ERROR || http->status == SOCK_DONE || http->status == NON_SOCK_ERROR) {
            http->dont_accept_clients = TRUE;
            unlock_rwlock(&http->rwlock, "http_check_disconnect: Unable to unlock rwlock (TRUE)");
            return TRUE;
        }
        #ifdef DROP_HTTP_NO_CLIENTS
        if (http->cache_entry != NULL && !http->cache_entry->is_full) {
            cache_remove(http->cache_entry, cache);
            http->cache_entry = NULL;
        }
        if (http->status == DOWNLOADING || http->status == AWAITING_REQUEST) {
            close(http->sock_fd);
        }
        http->dont_accept_clients = TRUE;
        unlock_rwlock(&http->rwlock, "http_check_disconnect: Unable to unlock rwlock (DROP_HTTP)");
        return TRUE;
        #endif
    }
    unlock_rwlock(&http->rwlock, "http_check_disconnect: Unable to unlock rwlock (FALSE)");
    return FALSE;
}

const char *get_host_error(int err_code, int *out_error) {
    const char *err_msg;
    switch (err_code) {
        case HOST_NOT_FOUND: err_msg = "Authoritative Answer, Host not found"; *out_error = HOST_NOT_FOUND_CUSTOM; break;
        case TRY_AGAIN: err_msg = "Non-Authoritative, Host not found, or SERVERFAIL"; *out_error = TRY_AGAIN_CUSTOM; break;
        case NO_RECOVERY: err_msg = "Non recoverable errors, FORMERR, REFUSED, NOTIMP"; *out_error = NO_RECOVERY_CUSTOM; break;
        case NO_DATA: err_msg = "Valid name, no data record of requested type"; *out_error = NO_DATA_CUSTOM; break;
        default: err_msg = "Unknown error"; break;
    }
    return err_msg;
}

int http_open_socket(const char *hostname, int port, int *out_error) {
    int err_code;
    struct hostent *server_host = getipnodebyname(hostname, AF_INET, 0, &err_code);
    if (server_host == NULL) {
        if (ERROR_LOG) fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(err_code, out_error));
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
        *out_error = INTERNAL_ERROR;
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
        if (ERROR_LOG) perror("open_http_socket: connect error");
        *out_error = CONNECTION_ERROR;
        close(sock_fd);
        return -1;
    }

    if (fcntl(sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("open_http_socket: fcntl error");
    }

    return sock_fd;
}

void parse_http_response_headers(http_t *entry) {
    int minor_version, status;
    const char *msg;
    size_t msg_len;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int headers_size = phr_parse_response(entry->data, entry->data_size, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);
    if (headers_size == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_http_response: Unable to parse http response headers\n");
        entry->status = NON_SOCK_ERROR;
        entry->error = CONNECTION_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        free(entry->data); entry->data = NULL;
        entry->is_response_complete = FALSE;
        return;
    }

    if (status != 0) entry->code = status; //request may be incomplete, but it managed to get status

    if (headers_size >= 0) entry->headers_size = headers_size;

    for (int i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Transfer-Encoding", strlen("Transfer-Encoding")) &&
        strings_equal_by_length(headers[i].value, headers[i].value_len, "chunked", strlen("chunked"))) {
            entry->response_type = HTTP_CHUNKED;
        }
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Content-Length", strlen("Content-Length"))) {
            entry->response_type = HTTP_CONTENT_LENGTH;
            entry->response_size = get_number_from_string_by_length(headers[i].value, headers[i].value_len);
            if (entry->response_size == -1) {
                entry->status = NON_SOCK_ERROR;
                entry->error = INTERNAL_ERROR;
                entry->is_response_complete = FALSE;
                entry->data_size = 0;
                free(entry->data); entry->data = NULL;
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
        entry->status = NON_SOCK_ERROR;
        entry->error = INTERNAL_ERROR;
        entry->is_response_complete = FALSE;
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;
        return;
    }

    if (entry->code == 200) {
        if (entry->cache_entry == NULL) {
            entry->cache_entry = cache_add(entry->host, entry->path, entry->data, entry->data_size, cache);
            if (entry->cache_entry == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            write_lock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_chunked: Unable to write-lock cache rwlock (code==200)");
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
            unlock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_chunked: Unable to unlock cache rwlock (code==200)");
        }
    }

    if (pret == 0) {
        if (entry->cache_entry != NULL) {
            write_lock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_chunked: Unable to write-lock cache rwlock (pret==0)");
            entry->cache_entry->is_full = TRUE;
            unlock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_chunked: Unable to unlock cache rwlock (pret==0)");
        }
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
            write_lock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_by_length: Unable to write-lock cache rwlock (code==200)");
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
            unlock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_by_length: Unable to unlock cache rwlock (code==200)");
        }
    }
    if (entry->data_size == entry->headers_size + entry->response_size) {
        if (entry->cache_entry != NULL) {
            write_lock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_by_length: Unable to write-lock cache rwlock (size)");
            entry->cache_entry->is_full = TRUE;
            unlock_rwlock(&entry->cache_entry->rwlock, "parse_http_response_by_length: Unable to unlock cache rwlock (size)");
        }
        entry->is_response_complete = TRUE;
    }
}

void http_read_data(http_t *entry, cache_t *cache) {
    char buf[BUF_SIZE];
    errno = 0;
    ssize_t bytes_read = recv(entry->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);

    write_lock_rwlock(&entry->rwlock, "http_read_data: Unable to write-lock rwlock");
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) {
            unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (EWOULDBLOCK)");
            return;
        }

        if (ERROR_LOG) perror("read_http_data: Unable to read from http socket");
        entry->status = SOCK_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;

        unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (bytes_read==-1)");
        return;
    }
    if (bytes_read == 0) {
        entry->status = SOCK_DONE;
        close(entry->sock_fd);
        if (entry->cache_entry != NULL && !entry->cache_entry->is_full) {
            cache_remove(entry->cache_entry, cache);
        }

        unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (bytes_read==0)");
        return;
    }

    if (entry->status != DOWNLOADING) {
        if (ERROR_LOG) fprintf(stderr, "read_http_data: reading from http when we shouldn't\n");
        if (INFO_LOG) write(STDERR_FILENO, buf, bytes_read);

        unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (!DOWNLOADING)");
        return;
    }

    char *check = (char *)realloc(entry->data, entry->data_size + BUF_SIZE);
    if (check == NULL) {
        if (ERROR_LOG) perror("read_http_data: Unable to reallocate memory for http data");
        entry->status = NON_SOCK_ERROR; entry->error = INTERNAL_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;

        unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (check==NULL)");
        return;
    }

    entry->data = check;
    memcpy(entry->data + entry->data_size, buf, bytes_read);
    entry->data_size += bytes_read;

    int b_no_headers = entry->headers_size == HTTP_NO_HEADERS;
    if (entry->headers_size == HTTP_NO_HEADERS) parse_http_response_headers(entry);
    if (entry->status == NON_SOCK_ERROR) {
        unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (NON_SOCK_ERROR)");
        return;
    }

    if (entry->headers_size >= 0) {
        if (entry->response_type == HTTP_CHUNKED) {
            parse_http_response_chunked(entry, buf, b_no_headers ? entry->headers_size : 0, b_no_headers ? entry->data_size - entry->headers_size : bytes_read, cache);
        }
        else if (entry->response_type == HTTP_CONTENT_LENGTH) {
            parse_http_response_by_length(entry, cache);
        }
    }

    unlock_rwlock(&entry->rwlock, "http_read_data: Unable to unlock rwlock (end)");
}

void http_send_request(http_t *entry) {
    ssize_t bytes_written = write(entry->sock_fd, entry->request + entry->request_bytes_written, entry->request_size - entry->request_bytes_written);

    if (bytes_written >= 0) entry->request_bytes_written += bytes_written;

    write_lock_rwlock(&entry->rwlock, "http_send_request: Unable to write-lock rwlock");
    if (entry->request_bytes_written == entry->request_size) {
        entry->status = DOWNLOADING;
        entry->request_size = 0;
        free(entry->request); entry->request = NULL;
    }

    if (bytes_written == -1) {
        if (ERROR_LOG) perror("http_send_request: unable to write to http socket");
        entry->status = SOCK_ERROR;
        close(entry->sock_fd);
        entry->request_size = 0;
        free(entry->request); entry->request = NULL;
    }
    unlock_rwlock(&entry->rwlock, "http_send_request: Unable to unlock rwlock");
}
