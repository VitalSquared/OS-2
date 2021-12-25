#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "client.h"
#include "list.h"

void create_client(int client_sock_fd, client_list_t *client_list) {
    client_t *new_client = (client_t *)calloc(1, sizeof(client_t));
    if (new_client == NULL) {
        if (ERROR_LOG) perror("create_client: Unable to allocate memory for client struct");
        close(client_sock_fd);
        return;
    }
    if (client_init(new_client, client_sock_fd) == -1) {
        close(client_sock_fd);
        return;
    }
    client_add_to_list(new_client, client_list);
    if (INFO_LOG) printf("[%d] Connected\n", client_sock_fd);
}

void remove_client(client_t *client, client_list_t *client_list) {
    client_remove_from_list(client, client_list);
    if (INFO_LOG) printf("[%d] Disconnected\n", client->sock_fd);
    client_destroy(client);
    free(client);
}

int client_init(client_t *client, int client_sock_fd) {
    client->sock_fd = client_sock_fd;
    client->status = AWAITING_REQUEST;
    client->cache_entry = NULL;
    client->http_entry = NULL;
    client->bytes_written = 0;
    client->request = NULL;
    client->request_size = 0;
    client->request_alloc_size = 0;

    if (fcntl(client_sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("create_client: fcntl error");
    }

    return 0;
}

void client_destroy(client_t *client) {
    if (client->http_entry != NULL) client->http_entry->clients--;
    close(client->sock_fd);
}

void client_goes_error(client_t *client) {
    client->status = SOCK_ERROR;
    if (client->http_entry != NULL) {
        client->http_entry->clients--;
        client->http_entry = NULL;
    }
    client->bytes_written = 0;
    client->request_size = 0;
    free_with_null((void **)&client->request);
}

void client_update_http_info(client_t *client) {
    if (client->http_entry != NULL) {
        if (IS_ERROR_STATUS(client->http_entry->status)) {
            client_goes_error(client);
        }
        else if (client->http_entry->cache_entry != NULL && client->http_entry->cache_entry->is_full) {
            client->status = GETTING_FROM_CACHE;
            client->cache_entry = client->http_entry->cache_entry;
            client->http_entry->clients--;
            client->http_entry = NULL;
        }
    }
}

int parse_client_request(client_t *client, char **host, char **path, ssize_t bytes_read) {
    const char *method, *phr_path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int err_code = phr_parse_request(client->request, client->request_size, &method, &method_len, &phr_path, &path_len, &minor_version, headers, &num_headers, client->request_size - bytes_read);
    if (err_code == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to parse request\n");
        client_goes_error(client);
        return -1;
    }
    if (err_code == -2) return -2; //incomplete, read from client more

    if (!strings_equal_by_length(method, method_len, "GET", 3)) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: not a GET method\n");
        client_goes_error(client);
        return -1;
    }

    *path = (char *)calloc(path_len + 1, sizeof(char));
    if (*path == NULL) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for path\n");
        client_goes_error(client);
        return -1;
    }
    memcpy(*path, phr_path, path_len);

    int found_host = FALSE;
    for (size_t i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len,  "Host", 4)) {
            *host = calloc(headers[i].value_len + 1, sizeof(char));
            if (*host == NULL) {
                if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for host\n");
                free_with_null((void **)path);
                client_goes_error(client);
                return -1;
            }
            memcpy(*host, headers[i].value, headers[i].value_len);
            found_host = TRUE;
            break;
        }
    }
    if (!found_host) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: no host header\n");
        free_with_null((void **)path);
        client_goes_error(client);
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read, http_list_t *http_list, cache_t *cache) {
    char *host = NULL, *path = NULL;
    int err_code = parse_client_request(client, &host, &path, bytes_read);
    if (err_code == -1) {
        client_goes_error(client);
        return;
    }
    if (err_code == -2) return; //incomplete request

    client->request_alloc_size = 0;

    cache_entry_t *entry = cache_find(host, path, cache);
    if (entry != NULL && entry->is_full) {
        if (INFO_LOG) printf("[%d] Getting data from cache for '%s%s'\n", client->sock_fd, host, path);
        client->status = GETTING_FROM_CACHE;
        client->cache_entry = entry;
        client->request_size = 0;
        free_with_null((void **)&client->request);
        free(host); free(path);
        return;
    }

    //there is no entry in cache:
    http_t *http_entry = http_list->head;
    while (http_entry != NULL) {    //we look for already existing http connection with the same request
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
            (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE)) {
            client->request_size = 0;
            free_with_null((void **)&client->request);
            http_entry->clients++;
            break;
        }
        http_entry = http_entry->next;
    }

    if (http_entry == NULL) {  //no active http entry with the same request
        int http_sock_fd = http_open_socket(host, 80);
        if (http_sock_fd == -1) {
            client_goes_error(client);
            free(host); free(path);
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path, http_list);
        if (http_entry == NULL) {
            client_goes_error(client);
            free(host); free(path);
            close(http_sock_fd);
            return;
        }

        client->request_size = 0;
        client->request = NULL;
    }

    client->status = DOWNLOADING;
    client->http_entry = http_entry;
    if (INFO_LOG) printf("[%d] No data in cache for '%s %s'.\n", client->sock_fd, host, path);
}

void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache) {
    char buf[BUF_SIZE + 1];
    errno = 0;
    ssize_t bytes_read = recv(client->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("client_read_data: Unable to read from client socket");
        client_goes_error(client);
        return;
    }
    if (bytes_read == 0) {
        client->status = SOCK_DONE;
        client->request_size = 0;
        free_with_null((void **)&client->request);
        return;
    }

    if (client->status != AWAITING_REQUEST) {
        if ((client->status == DOWNLOADING && client->bytes_written == client->http_entry->data_size) ||
            (client->status == GETTING_FROM_CACHE && client->bytes_written == client->cache_entry->size)) {
            if (client->http_entry != NULL) {
                client->http_entry->clients--;
                client->http_entry = NULL;
            }
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
            client->request_size = 0;
            client->request_alloc_size = 0;
            free_with_null((void **)&client->request);
        }
        else {
            if (ERROR_LOG) fprintf(stderr, "client_read_data: client read data when we shouldn't\n");
            /*if (INFO_LOG) {
                buf[bytes_read] = '\n';
                write(STDERR_FILENO, buf, bytes_read + 1);
            }*/
            return;
        }
    }

    if (client->request_size + bytes_read > client->request_alloc_size) {
        client->request_alloc_size += BUF_SIZE;
        char *check = (char *)realloc(client->request, client->request_alloc_size);
        if (check == NULL) {
            if (ERROR_LOG) perror("read_data_from_client: Unable to reallocate memory for client request");
            client_goes_error(client);
            return;
        }
        client->request = check;
    }

    memcpy(client->request + client->request_size, buf, bytes_read);
    client->request_size += bytes_read;

    handle_client_request(client, bytes_read, http_list, cache);
}

void check_finished_writing_to_client(client_t *client) {
    size_t size = 0;

    if (client->status == GETTING_FROM_CACHE) size = client->cache_entry->size;
    else if (client->status == DOWNLOADING) size = client->http_entry->data_size;

    if (client->bytes_written >= size && (
            (client->status == GETTING_FROM_CACHE && client->cache_entry->is_full) ||
            (client->status == DOWNLOADING && client->http_entry->is_response_complete))) {
        client->bytes_written = 0;

        client->cache_entry = NULL;
        if (client->http_entry != NULL) {
            client->http_entry->clients--;
            client->http_entry = NULL;
        }

        client->status = AWAITING_REQUEST;
    }
}

void write_to_client(client_t *client) {
    ssize_t offset = client->bytes_written;
    const char *buf = "";
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE) {
        buf = client->cache_entry->data;
        size = client->cache_entry->size;
    }
    else if (client->status == DOWNLOADING) {
        buf = client->http_entry->data;
        size = client->http_entry->data_size;
    }

    ssize_t bytes_written = write(client->sock_fd, buf + offset, size - offset);
    if (bytes_written == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("write_to_client: Unable to write to client socket");
        client_goes_error(client);
        return;
    }
    client->bytes_written += bytes_written;

    check_finished_writing_to_client(client);
}
