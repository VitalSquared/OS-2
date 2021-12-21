/*
 * Proxy, created by Vitaly Spirin, NSU, group 19203
 * This proxy uses picohttpparser: https://github.com/h2o/picohttpparser
 */

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "http.h"
#include "cache.h"
#include "states.h"

typedef struct client {
    int sock_fd, status, error;
    cache_entry_t *cache_entry;  http_t *http_entry;
    char *request;  ssize_t request_size;
    ssize_t bytes_written;
    struct client *prev, *next;
} client_t;

typedef struct client_list {
    client_t *head;
    client_t *tail;
} client_list_t;

typedef struct http_list_t {
    http_t *head;
    http_t *tail;
} http_list_t;

int listen_fd;
cache_t cache;

client_list_t client_queue = { .head = NULL, .tail = NULL };
pthread_mutex_t client_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t client_queue_cond = PTHREAD_COND_INITIALIZER;

http_list_t http_queue = { .head = NULL, .tail = NULL };
pthread_mutex_t http_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t http_queue_cond = PTHREAD_COND_INITIALIZER;

http_list_t global_http_list = { .head = NULL, .tail = NULL };
pthread_rwlock_t global_http_list_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void *client_worker(void *param);
void *http_worker(void *param);

int open_listen_socket(int port) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        if (ERROR_LOG) perror("open_listen_socket: socket error");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr_in)) == -1) {
        if (ERROR_LOG) perror("open_listen_socket: bind error");
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, SOMAXCONN) == -1) {
        if (ERROR_LOG) perror("open_listen_socket: listen error");
        close(sock_fd);
        return -1;
    }

    if (fcntl(sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("open_listen_socket: fcntl error");
    }

    return sock_fd;
}

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path) {
    http_t *new_http = (http_t *)calloc(1, sizeof(http_t));
    if (new_http == NULL) {
        if (ERROR_LOG) perror("create_http: Unable to allocate memory for http struct");
        return NULL;
    }

    if (http_init(new_http, sock_fd, request, request_size, host, path) == -1) {
        free(new_http);
        return NULL;
    }

    //add http to queue
    pthread_mutex_lock(&http_queue_mutex);

    new_http->prev = NULL;
    new_http->next = http_queue.head;
    http_queue.head = new_http;
    if (new_http->next != NULL) new_http->next->prev = new_http;
    if (http_queue.tail == NULL) http_queue.tail = http_queue.head;

    pthread_cond_broadcast(&http_queue_cond);
    pthread_mutex_unlock(&http_queue_mutex);

    if (INFO_LOG) printf("[%s %s] Connected\n", host, path);
    return new_http;
}

void remove_http(http_t *http, http_list_t *http_list) {
    //remove http from local list
    if (http == http_list->head) {
        http_list->head = http->next;
        if (http_list->head != NULL) http_list->head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (http->next != NULL) http->next->prev = http->prev;
    }

    //remove http from global list
    write_lock_rwlock(&global_http_list_rwlock, "remove_http: Unable to write-lock rwlock");
    if (http == global_http_list.head) {
        global_http_list.head = http->global_next;
        if (global_http_list.head != NULL) global_http_list.head->global_prev = NULL;
    }
    else {
        http->global_prev->global_next = http->global_next;
        if (http->global_next != NULL) http->global_next->global_prev = http->global_prev;
    }
    unlock_rwlock(&global_http_list_rwlock, "remove_http: Unable to unlock rwlock");

    if (INFO_LOG) printf("[%d %s %s] Disconnected\n", http->sock_fd, http->host, http->path);
    http_destroy(http);
    free(http);
}

void create_client(int client_sock_fd) {
    if (fcntl(client_sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("create_client: fcntl error");
    }

    client_t *new_client = (client_t *)calloc(1, sizeof(client_t));
    if (new_client == NULL) {
        if (ERROR_LOG) perror("create_client: Unable to allocate memory for client struct");
        close(client_sock_fd);
        return;
    }

    //init client data
    new_client->sock_fd = client_sock_fd;
    new_client->status = AWAITING_REQUEST;
    new_client->cache_entry = NULL;
    new_client->http_entry = NULL;
    new_client->bytes_written = 0;
    new_client->request = NULL;
    new_client->request_size = 0;

    //add client to queue
    pthread_mutex_lock(&client_queue_mutex);

    new_client->prev = NULL;
    new_client->next = client_queue.head;
    client_queue.head = new_client;
    if (new_client->next != NULL) new_client->next->prev = new_client;
    if (client_queue.tail == NULL) client_queue.tail = client_queue.head;

    pthread_cond_broadcast(&client_queue_cond);
    pthread_mutex_unlock(&client_queue_mutex);

    if (INFO_LOG) printf("[%d] Connected\n", client_sock_fd);
}

void remove_client(client_t *client, client_list_t *client_list) {
    if (client == client_list->head) {
        client_list->head = client->next;
        if (client_list->head != NULL) client_list->head->prev = NULL;
    }
    else {
        client->prev->next = client->next;
        if (client->next != NULL) client->next->prev = client->prev;
    }

    if (INFO_LOG) printf("[%d] Disconnected\n", client->sock_fd);

    if (client->http_entry != NULL) {
        write_lock_rwlock(&client->http_entry->rwlock, "remove_client: Unable to write-lock rwlock");
        client->http_entry->clients--;
        unlock_rwlock(&client->http_entry->rwlock, "remove_client: Unable to unlock rwlock");
    }
    close(client->sock_fd);
    free(client);
}

void remove_all_queued_connections() {
    client_t *client = client_queue.head;
    while (client != NULL) {
        client_t *next = client->next;
        if (INFO_LOG) printf("[%d] Disconnected\n", client->sock_fd);
        close(client->sock_fd);
        free(client);
        client = next;
    }

    http_t *http = http_queue.head;
    while (http != NULL) {
        http_t *next = http->next;
        if (INFO_LOG) printf("[%d %s %s] Disconnected\n", http->sock_fd, http->host, http->path);
        http_destroy(http);
        free(http);
        http = next;
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
        client->status = NON_SOCK_ERROR; client->error = INVALID_REQUEST;
        client->bytes_written = 0;
        return -1;
    }
    if (err_code == -2) return -2; //incomplete, read from client more

    if (!strings_equal_by_length(method, method_len, "GET", 3)) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: not a GET method\n");
        client->status = NON_SOCK_ERROR; client->error = NOT_A_GET_METHOD;
        client->bytes_written = 0;
        return -1;
    }

    *path = (char *)calloc(path_len + 1, sizeof(char));
    if (*path == NULL) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for path\n");
        client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
        client->bytes_written = 0;
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
                client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
                client->bytes_written = 0;
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
        client->status = NON_SOCK_ERROR; client->error = INVALID_REQUEST;
        client->bytes_written = 0;
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read) {
    char *host = NULL, *path = NULL;
    int err_code = parse_client_request(client, &host, &path, bytes_read);
    if (err_code == -1) {
        client->request_size = 0;
        free(client->request); client->request = NULL;
        return;
    }
    if (err_code == -2) return;

    cache_entry_t *cache_entry = cache_find(host, path, &cache);
    if (cache_entry != NULL){
        read_lock_rwlock(&cache_entry->rwlock, "handle_client_request: Unable to read-lock cache rwlock");
        if (cache_entry->is_full) {
            unlock_rwlock(&cache_entry->rwlock, "handle_client_request: Unable to unlock cache rwlock (full)");
            if (INFO_LOG) printf("[%d] Getting data from cache for '%s%s'\n", client->sock_fd, host, path);
            client->status = GETTING_FROM_CACHE;
            client->cache_entry = cache_entry;
            client->request_size = 0;
            free(client->request); client->request = NULL;
            free(host); free(path);
            return;
        }
        unlock_rwlock(&cache_entry->rwlock, "handle_client_request: Unable to unlock cache rwlock (non-full)");
    }

    //there is no cache_entry in cache:
    //search for queued https first
    pthread_mutex_lock(&http_queue_mutex);

    http_t *http_entry = http_queue.head;
    while (http_entry != NULL) {
        read_lock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to read-lock http_entry rwlock");

        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
        (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE) && !http_entry->dont_accept_clients) {   //there is active http
            http_entry->clients++;
            unlock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to unlock http_entry rwlock (found)");

            client->request_size = 0;
            free(client->request); client->request = NULL;
            break;
        }

        unlock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to unlock http_entry rwlock (not found)");
        http_entry = http_entry->next;
    }

    pthread_mutex_unlock(&http_queue_mutex);

    if (http_entry == NULL) {
        //there is no queued https
        read_lock_rwlock(&global_http_list_rwlock, "handle_client_request: Unable to read-lock http_list rwlock");

        http_entry = global_http_list.head;
        while (http_entry != NULL) {    //we look for already existing http connection with the same request
            read_lock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to read-lock http_entry rwlock");

            if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
            (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE) && !http_entry->dont_accept_clients) {   //there is active http
                http_entry->clients++;
                unlock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to unlock http_entry rwlock (found)");

                client->request_size = 0;
                free(client->request); client->request = NULL;
                break;
            }

            unlock_rwlock(&http_entry->rwlock, "handle_client_request: Unable to unlock http_entry rwlock (not found)");
            http_entry = http_entry->next;
        }

        unlock_rwlock(&global_http_list_rwlock, "handle_client_request: Unable to unlock http_list rwlock");
    }

    if (http_entry == NULL)  {  //no active http cache_entry with the same request
        int http_sock_fd = http_open_socket(host, 80, &err_code);
        if (http_sock_fd == -1) {
            client->status = NON_SOCK_ERROR; client->error = err_code;
            client->bytes_written = 0;
            client->request_size = 0;
            free(client->request); client->request = NULL;
            free(host); free(path);
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path);
        if (http_entry == NULL) {
            client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
            client->bytes_written = 0;
            client->request_size = 0;
            free(client->request); client->request = NULL;
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

void read_data_from_client(client_t *client) {
    char buf[BUF_SIZE + 1];
    errno = 0;
    ssize_t bytes_read = recv(client->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("read_data_from_client: Unable to read from client socket");
        client->status = SOCK_ERROR;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }
    if (bytes_read == 0) {
        client->status = SOCK_DONE;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }

    if (client->status != AWAITING_REQUEST) {
        int error = TRUE;
        if (client->status == DOWNLOADING) {
            write_lock_rwlock(&client->http_entry->rwlock, "read_data_from_client: Unable to write-lock http_entry rwlock");
            if (client->bytes_written == client->http_entry->data_size) {
                client->http_entry->clients--;
                unlock_rwlock(&client->http_entry->rwlock, "read_data_from_client: Unable to unlock http_entry rwlock (good)");
                client->http_entry = NULL;
                client->bytes_written = 0;
                client->status = AWAITING_REQUEST;
                client->request_size = 0;
                free(client->request);  client->request = NULL;
                error = FALSE;
            }
            if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "read_data_from_client: Unable to write-lock http_entry rwlock (bad)");
        }
        else if (client->status == GETTING_FROM_CACHE){
            read_lock_rwlock(&client->cache_entry->rwlock, "read_data_from_client: Unable to read-lock cache_entry rwlock");
            if (client->bytes_written == client->cache_entry->size) {
                unlock_rwlock(&client->cache_entry->rwlock, "read_data_from_client: Unable to unlock cache_entry rwlock (good)");
                client->cache_entry = NULL;
                client->bytes_written = 0;
                client->status = AWAITING_REQUEST;
                client->request_size = 0;
                free(client->request);  client->request = NULL;
                error = FALSE;
            }
            if (client->cache_entry != NULL) unlock_rwlock(&client->cache_entry->rwlock, "read_data_from_client: Unable to unlock cache_entry rwlock (bad)");
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
        client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
        client->bytes_written = 0;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }

    client->request = check;
    memcpy(client->request + client->request_size, buf, bytes_read);
    client->request_size += bytes_read;

    handle_client_request(client, bytes_read);
}

void check_finished_writing_to_client(client_t *client) {
    if (client->status == DOWNLOADING) {
        write_lock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: Unable to write-lock http_entry rwlock");
        if (client->bytes_written >= client->http_entry->data_size && client->http_entry->is_response_complete) {
            client->http_entry->clients--;
            unlock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: Unable to unlock http_entry rwlock (if)");
            client->http_entry = NULL;
            client->bytes_written = 0;
            client->cache_entry = NULL;
            client->status = AWAITING_REQUEST;
        }
        if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "check_finished_writing_to_client: Unable to unlock http_entry rwlock (not if)");
    }
    else if (client->status == GETTING_FROM_CACHE) {
        read_lock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: Unable to read-lock cache_entry rwlock");
        if (client->bytes_written >= client->cache_entry->size && client->cache_entry->is_full) {
            unlock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: Unable to unlock cache_entry rwlock (if)");
            client->cache_entry = NULL;
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
        }
        if (client->cache_entry != NULL) unlock_rwlock(&client->cache_entry->rwlock, "check_finished_writing_to_client: Unable to unlock cache_entry rwlock (not if)");
    }
}

void write_to_client(client_t *client) {
    ssize_t offset = client->bytes_written;
    const char *buf = "";
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE) {
        read_lock_rwlock(&client->cache_entry->rwlock, "write_to_client: Unable to read-lock cache_entry rwlock");
        buf = client->cache_entry->data;
        size = client->cache_entry->size;
        unlock_rwlock(&client->cache_entry->rwlock, "write_to_client: Unable to unlock cache_entry rwlock");
    }
    else if (client->status == DOWNLOADING) {
        read_lock_rwlock(&client->http_entry->rwlock, "write_to_client: Unable to read-lock http_entry rwlock");
        if (client->http_entry->data == NULL) {
            unlock_rwlock(&client->http_entry->rwlock, "write_to_client: Unable to unlock http_entry rwlock");
            return;
        }
        buf = client->http_entry->data;
        size = client->http_entry->data_size;
        unlock_rwlock(&client->http_entry->rwlock, "write_to_client: Unable to unlock http_entry rwlock");
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

void *client_cancel_handler(void *param) {
    client_list_t *client_list = (client_list_t *)param;
    if (client_list == NULL) {
        if (ERROR_LOG) fprintf(stderr, "client_cancel_handler: param was NULL\n");
        return NULL;
    }

    client_t *client = client_list->head;
    while (client != NULL) {
        client_t *next = client->next;
        remove_client(client, client_list);
        client = next;
    }

    return NULL;
}

void *client_worker(void *param) {
    client_list_t client_list = { .head = NULL };

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(client_cancel_handler, &client_list);

    fd_set readfds, writefds;
    while (TRUE) {
        //get new client from queue
        pthread_mutex_lock(&client_queue_mutex);

        while (client_queue.head == NULL) {
            if (client_list.head != NULL) break;
            pthread_cond_wait(&client_queue_cond, &client_queue_mutex);
        }

        if (client_queue.head != NULL) {
            client_t *new_client = client_queue.tail;
            client_queue.tail = client_queue.tail->prev;
            if (client_queue.tail != NULL) client_queue.tail->next = NULL;
            else client_queue.head = NULL;

            new_client->prev = NULL;
            new_client->next = client_list.head;
            client_list.head = new_client;
            if (new_client->next != NULL) new_client->next->prev = new_client;
        }

        pthread_mutex_unlock(&client_queue_mutex);

        int select_max_fd = 0;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        client_t *client = client_list.head;
        while (client != NULL) {
            client_t *next = client->next;

            if (client->status == SOCK_ERROR || client->status == SOCK_DONE || client->status == NON_SOCK_ERROR) {
                remove_client(client, &client_list);
                client = next;
                continue;
            }

            if (client->http_entry != NULL) {
                write_lock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to write-lock http_entry rwlock (pre-FD_SET)");
                if (client->http_entry->status == NON_SOCK_ERROR || client->http_entry->status == SOCK_ERROR) {
                    client->status = NON_SOCK_ERROR;
                    client->error = client->http_entry->status == SOCK_ERROR ? CONNECTION_ERROR : client->http_entry->error;
                    client->http_entry->clients--;
                    unlock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to unlock http_entry rwlock (pre-FD_SET null error)");
                    client->http_entry = NULL;
                    client->bytes_written = 0;
                }
                else if (client->http_entry->cache_entry != NULL && client->http_entry->cache_entry->is_full) {
                    client->http_entry->clients--;
                    client->cache_entry = client->http_entry->cache_entry;
                    unlock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to unlock http_entry rwlock (pre-FD_SET null cache)");
                    client->http_entry = NULL;
                    client->status = GETTING_FROM_CACHE;
                }
                if (client->http_entry != NULL) unlock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to unlock http_entry rwlock (pre-FD_SET non-null)");
            }
            check_finished_writing_to_client(client);

            select_max_fd = MAX(client->sock_fd, select_max_fd);

            FD_SET(client->sock_fd, &readfds);
            if (client->status == DOWNLOADING) {
                read_lock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to read-lock http_entry rwlock (FD_SET)");
                if (client->bytes_written < client->http_entry->data_size) {
                    FD_SET(client->sock_fd, &writefds);
                }
                unlock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to unlock http_entry rwlock (FD_SET)");
            }
            else if (client->status == GETTING_FROM_CACHE) {
                read_lock_rwlock(&client->cache_entry->rwlock, "client_worker: Unable to read-lock cache_entry rwlock (FD_SET)");
                if (client->bytes_written < client->cache_entry->size) {
                    FD_SET(client->sock_fd, &writefds);
                }
                unlock_rwlock(&client->cache_entry->rwlock, "client_worker: Unable to unlock cache_entry rwlock (FD_SET)");
            }

            client = next;
        }

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, &timeout);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) fprintf(stderr, "client_worker: select error\n");
            break;
        }
        if (num_fds_ready == 0) continue;

        client = client_list.head;
        while (client != NULL) {
            client_t *next = client->next;

            if (client->status != SOCK_ERROR && client->status != SOCK_DONE && client->status != NON_SOCK_ERROR && FD_ISSET(client->sock_fd, &readfds)) {
                read_data_from_client(client);
            }

            if (FD_ISSET(client->sock_fd, &writefds)) {
                ssize_t http_data_size = 0;
                int http_status;
                if (client->http_entry != NULL) {
                    read_lock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to read-lock http_entry rwlock (post select)");
                    http_data_size = client->http_entry->data_size;
                    http_status = client->http_entry->status;
                    unlock_rwlock(&client->http_entry->rwlock, "client_worker: Unable to unlock http_entry rwlock (post select)");
                }

                ssize_t cache_data_size = 0;
                if (client->cache_entry != NULL) {
                    read_lock_rwlock(&client->cache_entry->rwlock, "client_worker: Unable to read-lock cache_entry rwlock (post select)");
                    cache_data_size = client->cache_entry->size;
                    unlock_rwlock(&client->cache_entry->rwlock, "client_worker: Unable to unlock cache_entry rwlock (post select)");
                }

                if (((client->status == DOWNLOADING && http_status == DOWNLOADING && client->bytes_written < http_data_size) ||
                    (client->status == GETTING_FROM_CACHE && client->bytes_written < cache_data_size))) {
                    write_to_client(client);
                }
            }

            client = next;
        }
    }
    pthread_cleanup_pop(TRUE);

    return NULL;
}

void *http_cancel_handler(void *param) {
    http_list_t *http_list= (http_list_t *)param;
    if (http_list == NULL) {
        if (ERROR_LOG) fprintf(stderr, "http_cancel_handler: param was NULL\n");
        return NULL;
    }

    http_t *http = http_list->head;
    while (http != NULL) {
        http_t *next = http->next;
        remove_http(http, http_list);
        http = next;
    }

    return NULL;
}

void *http_worker(void *param) {
    http_list_t http_list = { .head = NULL };

    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(http_cancel_handler, &http_list);

    fd_set readfds, writefds;
    while (TRUE) {
        //get new http from queue
        pthread_mutex_lock(&http_queue_mutex);

        while (http_queue.head == NULL) {
            if (http_list.head != NULL) break;
            pthread_cond_wait(&http_queue_cond, &http_queue_mutex);
        }

        if (http_queue.head != NULL) {
            http_t *new_http = http_queue.tail;
            http_queue.tail = http_queue.tail->prev;
            if (http_queue.tail != NULL) http_queue.tail->next = NULL;
            else http_queue.head = NULL;

            new_http->prev = NULL;
            new_http->next = http_list.head;
            http_list.head = new_http;
            if (new_http->next != NULL) new_http->next->prev = new_http;

            write_lock_rwlock(&global_http_list_rwlock, "http_worker: Unable to write-lock http list rwlock");
            new_http->global_prev = NULL;
            new_http->global_next = global_http_list.head;
            global_http_list.head = new_http;
            if (new_http->global_next != NULL) new_http->global_next->global_prev = new_http;
            unlock_rwlock(&global_http_list_rwlock, "http_worker: Unable to write-lock http list rwlock");
        }

        pthread_mutex_unlock(&http_queue_mutex);

        int select_max_fd = 0;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        http_t *http = http_list.head;
        while (http != NULL) {
            http_t *next = http->next;
            if (http_check_disconnect(http, &cache)) {
                remove_http(http, &http_list);
                http = next;
                continue;
            }
            select_max_fd = MAX(http->sock_fd, select_max_fd);
            if (http->status != SOCK_ERROR && http->status != SOCK_DONE && http->status != NON_SOCK_ERROR) {
                FD_SET(http->sock_fd, &readfds);
            }
            if (http->status == AWAITING_REQUEST) {
                FD_SET(http->sock_fd, &writefds);
            }
            http = next;
        }

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, &timeout);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) fprintf(stderr, "http_worker: select error\n");
            break;
        }
        if (num_fds_ready == 0) continue;

        http = http_list.head;
        while (http != NULL) {
            http_t *next = http->next;
            if (http->status != SOCK_ERROR && http->status != SOCK_DONE && http->status != NON_SOCK_ERROR && FD_ISSET(http->sock_fd, &readfds)) {
                http_read_data(http, &cache);
            }
            if (http->status == AWAITING_REQUEST && FD_ISSET(http->sock_fd, &writefds)) {
                http_send_request(http);
            }
            http = next;
        }
    }
    pthread_cleanup_pop(TRUE);

    return NULL;
}

void update_accept(fd_set *readfds) {
    if (FD_ISSET(listen_fd, readfds)) {
        errno = 0;
        int client_sock_fd = accept(listen_fd, NULL, NULL);
        if (client_sock_fd == -1) {
            if (errno == EWOULDBLOCK) {
                return;
            }
            if (ERROR_LOG) perror("update_accept: accept error");
            return;
        }
        create_client(client_sock_fd);
    }
}

int update_stdin(fd_set *readfds) {
    if (FD_ISSET(STDIN_FILENO, readfds)) {
        char buf[BUF_SIZE + 1];
        ssize_t bytes_read = read(STDIN_FILENO, buf, BUF_SIZE);
        if (bytes_read == -1) {
            if (ERROR_LOG)  perror("main: Unable to read from stdin");
            return -1;
        }
        buf[bytes_read] = '\0';
        if (buf[bytes_read - 1] == '\n') buf[bytes_read - 1] = '\0';

        if (STR_EQ(buf, "exit")) return -1;
        else if (STR_EQ(buf, "cache")) cache_print_content(&cache);
    }
    return 0;
}

void proxy_spin() {
    fd_set readfds;
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    while (TRUE) {
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int num_fds_ready = select(listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) perror("proxy_spin: select error");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_accept(&readfds);
        if (update_stdin(&readfds) == -1) break;
    }
}

int parse_args(char *listen_port_str, int *listen_port, char *clients_pool_str, int *clients_pool, char *https_pool_str, int *https_pool) {
    if (convert_number(listen_port_str, listen_port) == -1 ||
        convert_number(clients_pool_str, clients_pool) == -1 ||
        convert_number(https_pool_str, https_pool) == -1 ) {
        return -1;
    }
    if (!IS_PORT_VALID(*listen_port) || !IS_POOL_SIZE_VALID(*clients_pool) || !IS_POOL_SIZE_VALID(*https_pool)) {
        if (ERROR_LOG) fprintf(stderr, "Invalid port: listen_port=%d, clients_pool=%d, https_pool=%d\n", *listen_port, *clients_pool, *https_pool);
        return -1;
    }
    return 0;
}

void cleanup() {
    cache_destroy(&cache);
    pthread_mutex_destroy(&client_queue_mutex);
    pthread_cond_destroy(&client_queue_cond);
    pthread_mutex_destroy(&http_queue_mutex);
    pthread_cond_destroy(&http_queue_cond);
    pthread_rwlock_destroy(&global_http_list_rwlock);
    close(listen_fd);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s listen_port clients_pool_size https_pool_size\n", argv[0]);
        return EXIT_SUCCESS;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("main: signal error");
        return EXIT_FAILURE;
    }
    if (cache_init(&cache) != 0) {
        fprintf(stderr, "Unable to init cache\n");
        return EXIT_FAILURE;
    }

    int port, clients_pool, https_pool;
    if (parse_args(argv[1], &port, argv[2], &clients_pool, argv[3], &https_pool) == -1) return EXIT_FAILURE;
    if ((listen_fd = open_listen_socket(port)) == -1) return EXIT_FAILURE;
    atexit(cleanup);

    int err_code, clients_created = 0, https_created = 0;
    pthread_t clients_threads[clients_pool];
    pthread_t https_threads[https_pool];

    for (int i = 0; i < clients_pool; i++) {
        err_code = pthread_create(&clients_threads[i], NULL, client_worker, NULL);
        if (err_code != 0) {
            print_error("Unable to create client thread\n", err_code);
            break;
        }
        clients_created++;
    }
    fprintf(stderr, "Created %d out of %d client threads!\n", clients_created, clients_pool);
    if (clients_created == 0) return EXIT_FAILURE;

    for (int i = 0; i < https_pool; i++) {
        err_code = pthread_create(&https_threads[i], NULL, http_worker, NULL);
        if (err_code != 0) {
            print_error("Unable to create http thread\n", err_code);
            break;
        }
        https_created++;
    }
    fprintf(stderr, "Created %d out of %d http threads!\n", https_created, https_pool);
    if (https_created == 0) {
        for (int i = 0; i < clients_created; i++) pthread_cancel(clients_threads[i]);
        pthread_exit(NULL);
    }

    proxy_spin();

    for (int i = 0; i < clients_created; i++) pthread_cancel(clients_threads[i]);
    for (int i = 0; i < https_created; i++) pthread_cancel(https_threads[i]);
    remove_all_queued_connections();
    pthread_exit(NULL);
}
