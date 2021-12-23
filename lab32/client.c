#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include "client.h"
#include "list.h"

void create_client(int client_sock_fd, client_list_t *client_list, void *(*thread_func)(void *)) {
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
    int err_code = pthread_create(&new_client->thread_id, NULL, thread_func, new_client);
    if (err_code != 0) {
        if (ERROR_LOG) print_error("create_client: Unable to create thread", err_code);
        close(client_sock_fd);
        free(new_client);
        return;
    }
    pthread_detach(new_client->thread_id);
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

    if (fcntl(client_sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("create_client: fcntl error");
    }

    return 0;
}

void client_destroy(client_t *client) {
    if (client->http_entry != NULL) {
        write_lock_rwlock(&client->http_entry->rwlock, "client_destroy");
        client->http_entry->clients--;
        unlock_rwlock(&client->http_entry->rwlock, "client_destroy");
    }
    close(client->sock_fd);
}

void client_goes_error(client_t *client) {
    client->status = SOCK_ERROR;
    if (client->http_entry != NULL) {
        write_lock_rwlock(&client->http_entry->rwlock, "client_goes_error");
        client->http_entry->clients--;
        char buf[1] = { 1 };
        write(client->http_entry->client_pipe_fd, buf, 1);
        unlock_rwlock(&client->http_entry->rwlock, "client_goes_error");
        client->http_entry = NULL;
    }
    client->bytes_written = 0;
    client->request_size = 0;
    free_with_null((void **)&client->request);
}

void client_update_http_info(client_t *client) {
    if (client->http_entry != NULL) {
        write_lock_rwlock(&client->http_entry->rwlock, "client_update_http_info");
        if (IS_ERROR_STATUS(client->http_entry->status)) {
            unlock_rwlock(&client->http_entry->rwlock, "client_update_http_info: ERROR STATUS");
            client_goes_error(client);
        }
        else if (client->http_entry->cache_entry != NULL && client->http_entry->cache_entry->is_full) {
            client->http_entry->clients--;
            char buf[1] = { 1 };
            write(client->http_entry->client_pipe_fd, buf, 1);
            client->cache_entry = client->http_entry->cache_entry;
            unlock_rwlock(&client->http_entry->rwlock, "client_update_http_info: FULL CACHE");
            client->http_entry = NULL;
            client->status = GETTING_FROM_CACHE;
        }
        if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "client_update_http_info");
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
                free(*path); *path = NULL;
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
        free(*path); *path = NULL;
        client_goes_error(client);
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read, http_list_t *http_list, cache_t *cache, void *(*http_thread_func)(void*)) {
    char *host = NULL, *path = NULL;
    int err_code = parse_client_request(client, &host, &path, bytes_read);
    if (err_code == -1) {
        client_goes_error(client);
        return;
    }
    if (err_code == -2) return;

    cache_entry_t *cache_entry = cache_find(host, path, cache);
    if (cache_entry != NULL) {
        read_lock_rwlock(&cache_entry->rwlock, "handle_client_request: CACHE");
        if (cache_entry->is_full) {
            unlock_rwlock(&cache_entry->rwlock, "handle_client_request: FULL CACHE");
            if (INFO_LOG) printf("[%d] Getting data from cache for '%s%s'\n", client->sock_fd, host, path);
            client->status = GETTING_FROM_CACHE;
            client->cache_entry = cache_entry;
            client->request_size = 0;
            free_with_null((void **)&client->request);
            free(host); free(path);
            return;
        }
        unlock_rwlock(&cache_entry->rwlock, "handle_client_request: CACHE");
    }

    //there is no cache_entry in cache:
    read_lock_rwlock(&http_list->rwlock, "handle_client_request: HTTP LIST");
    http_t *http_entry = http_list->head;
    while (http_entry != NULL) {    //we look for already existing http connection with the same request
        read_lock_rwlock(&http_entry->rwlock, "handle_client_request: HTTP ENTRY");
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
            (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE) && !http_entry->dont_accept_clients) {   //there is active http
            http_entry->clients++;
            char buf1[1] = { 1 };
            write(http_entry->client_pipe_fd, buf1, 1);
            unlock_rwlock(&http_entry->rwlock, "handle_client_request: HTTP ENTRY FOUND");
            client->request_size = 0;
            free_with_null((void **)&client->request);
            break;
        }
        unlock_rwlock(&http_entry->rwlock, "handle_client_request: HTTP ENTRY");
        http_entry = http_entry->next;
    }
    unlock_rwlock(&http_list->rwlock, "handle_client_request: HTTP LIST");

    if (http_entry == NULL)  {  //no active http cache_entry with the same request
        int http_sock_fd = http_open_socket(host, 80);
        if (http_sock_fd == -1) {
            client_goes_error(client);
            free(host); free(path);
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path, http_list, http_thread_func);
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

void client_read_data(client_t *client, http_list_t *http_list, cache_t *cache, void *(*http_thread_func)(void*)) {
    char buf[BUF_SIZE + 1];
    errno = 0;
    ssize_t bytes_read = recv(client->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("read_data_from_client: Unable to read from client socket");
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
        int error = TRUE;
        if (client->status == DOWNLOADING) {
            write_lock_rwlock(&client->http_entry->rwlock, "client_read_data: HTTP ENTRY");
            if (client->bytes_written == client->http_entry->data_size) {
                client->http_entry->clients--;
                char buf1[1] = { 1 };
                write(client->http_entry->client_pipe_fd, buf1, 1);
                unlock_rwlock(&client->http_entry->rwlock, "client_read_data: HTTP ENTRY EQUALS");
                client->http_entry = NULL;
                client->bytes_written = 0;
                client->status = AWAITING_REQUEST;
                client->request_size = 0;
                free_with_null((void **)&client->request);
                error = FALSE;
            }
            if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "client_read_data: HTTP ENTRY");
        }
        else if (client->status == GETTING_FROM_CACHE){
            read_lock_rwlock(&client->cache_entry->rwlock, "client_read_data: CACHE ENTRY");
            if (client->bytes_written == client->cache_entry->size) {
                unlock_rwlock(&client->cache_entry->rwlock, "client_read_data: CACHE ENTRY EQUALS");
                client->cache_entry = NULL;
                client->bytes_written = 0;
                client->status = AWAITING_REQUEST;
                client->request_size = 0;
                free_with_null((void **)&client->request);
                error = FALSE;
            }
            if (client->cache_entry != NULL) unlock_rwlock(&client->cache_entry->rwlock, "client_read_data: CACHE ENTRY");
        }

        if (error) {
            if (ERROR_LOG) fprintf(stderr, "read_data_from_client: client read data when we shouldn't\n");
            /*if (INFO_LOG) {
                buf[bytes_read] = '\n';
                write(STDERR_FILENO, buf, bytes_read + 1);
            }*/
            return;
        }
    }

    char *check = (char *)realloc(client->request, client->request_size + BUF_SIZE);
    if (check == NULL) {
        if (ERROR_LOG) perror("read_data_from_client: Unable to reallocate memory for client request");
        client_goes_error(client);
        return;
    }

    client->request = check;
    memcpy(client->request + client->request_size, buf, bytes_read);
    client->request_size += bytes_read;

    handle_client_request(client, bytes_read, http_list, cache, http_thread_func);
}

void check_finished_writing_to_client(client_t *client) {
    if (client->status == DOWNLOADING) {
        write_lock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: HTTP");
        if (client->bytes_written >= client->http_entry->data_size && client->http_entry->is_response_complete) {
            client->http_entry->clients--;
            char buf[1] = { 1 };
            write(client->http_entry->client_pipe_fd, buf, 1);
            unlock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: HTTP COMPLETE");
            client->http_entry = NULL;
            client->bytes_written = 0;
            client->cache_entry = NULL;
            client->status = AWAITING_REQUEST;
        }
        if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: HTTP");
    }
    else if (client->status == GETTING_FROM_CACHE) {
        read_lock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: CACHE");
        if (client->bytes_written >= client->cache_entry->size && client->cache_entry->is_full) {
            unlock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: CACHE COMPLETE");
            client->cache_entry = NULL;
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
        }
        if (client->cache_entry != NULL) unlock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: CACHE");
    }
}

void write_to_client(client_t *client) {
    ssize_t offset = client->bytes_written;
    const char *buf = "";
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE) {
        read_lock_rwlock(&client->cache_entry->rwlock, "write_to_client: CACHE");
        buf = client->cache_entry->data;
        size = client->cache_entry->size;
        unlock_rwlock(&client->cache_entry->rwlock, "write_to_client: CACHE");
    }
    else if (client->status == DOWNLOADING) {
        read_lock_rwlock(&client->http_entry->rwlock, "write_to_client: HTTP");
        if (client->http_entry->data == NULL) {
            unlock_rwlock(&client->http_entry->rwlock, "write_to_client: HTTP return");
            return;
        }
        buf = client->http_entry->data;
        size = client->http_entry->data_size;
        unlock_rwlock(&client->http_entry->rwlock, "write_to_client: HTTP");
    }

    ssize_t bytes_written = write(client->sock_fd, buf + offset, size - offset);
    if (bytes_written == -1) {
        if (ERROR_LOG) perror("write_to_client: Unable to write to client socket");
        client->status = SOCK_ERROR;
        return;
    }
    client->bytes_written += bytes_written;
    check_finished_writing_to_client(client);
}
