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
} client_list_t;

typedef struct http_list {
    http_t *head;
} http_list_t;

int listen_fd;
cache_t cache;

fd_set readfds, writefds;
int select_max_fd = STDIN_FILENO;

client_list_t client_list = { .head = NULL };
http_list_t http_list = { .head = NULL };

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

    //add http to list
    new_http->prev = NULL;
    new_http->next = http_list.head;
    http_list.head = new_http;
    if (new_http->next != NULL) new_http->next->prev = new_http;

    if (INFO_LOG) printf("[%s %s] Connected\n", host, path);
    return new_http;
}

void remove_http(http_t *http) {
    //remove http from list
    if (http == http_list.head) {
        http_list.head = http->next;
        if (http_list.head != NULL) http_list.head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (http->next != NULL) http->next->prev = http->prev;
    }

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
    select_max_fd = MAX(select_max_fd, client_sock_fd);

    //add client to list
    new_client->prev = NULL;
    new_client->next = client_list.head;
    client_list.head = new_client;
    if (new_client->next != NULL) new_client->next->prev = new_client;

    //init client data
    new_client->sock_fd = client_sock_fd;
    new_client->status = AWAITING_REQUEST;
    new_client->cache_entry = NULL;
    new_client->http_entry = NULL;
    new_client->bytes_written = 0;
    new_client->request = NULL;
    new_client->request_size = 0;

    if (INFO_LOG) printf("[%d] Connected\n", client_sock_fd);
}

void remove_client(client_t *client) {
    if (client == client_list.head) {
        client_list.head = client->next;
        if (client_list.head != NULL) client_list.head->prev = NULL;
    }
    else {
        client->prev->next = client->next;
        if (client->next != NULL) client->next->prev = client->prev;
    }

    if (INFO_LOG) printf("[%d] Disconnected\n", client->sock_fd);

    if (client->http_entry != NULL) client->http_entry->clients--;
    close(client->sock_fd);
    free(client);
}

void remove_all_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        remove_client(cur_client);
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        if (cur_http->status == DOWNLOADING) close(cur_http->sock_fd);
        remove_http(cur_http);
        cur_http = next;
    }
}

void print_active_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        printf("[cli %d] status=%d\n", cur_client->sock_fd, cur_client->status);
        if (cur_client->cache_entry != NULL) {
            printf("- cache=%s %s, size=%zd, bytes_written=%zd\n", cur_client->cache_entry->host, cur_client->cache_entry->path, cur_client->cache_entry->size, cur_client->bytes_written);
        }
        if (cur_client->http_entry != NULL) {
            printf("- http=%d %s %s, size=%zd, bytes_written=%zd\n", cur_client->http_entry->sock_fd, cur_client->http_entry->host, cur_client->http_entry->path, cur_client->http_entry->data_size, cur_client->bytes_written);
        }
        cur_client = cur_client->next;
    }
    printf("\n");
    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        printf("[http %d] status=%d, code=%d, clients=%d, is_response_complete=%d, response_type=%d\n", cur_http->sock_fd, cur_http->status, cur_http->code, cur_http->clients, cur_http->is_response_complete, cur_http->response_type);
        if (cur_http->cache_entry != NULL) {
            printf("- cache=%s %s, size=%zd\n", cur_http->cache_entry->host, cur_http->cache_entry->path, cur_http->cache_entry->size);
        }
        cur_http = cur_http->next;
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

    cache_entry_t *entry = cache_find(host, path, &cache);
    if (entry != NULL && entry->is_full) {
        if (INFO_LOG) printf("[%d] Getting data from cache for '%s%s'\n", client->sock_fd, host, path);
        client->status = GETTING_FROM_CACHE;
        client->cache_entry = entry;
        client->request_size = 0;
        free(client->request); client->request = NULL;
        free(host); free(path);
        return;
    }

    //there is no entry in cache:
    http_t *http_entry = http_list.head;
    while (http_entry != NULL) {    //we look for already existing http connection with the same request
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
            (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE)) {
            client->request_size = 0;
            free(client->request); client->request = NULL;
            http_entry->clients++;
            break;
        }
        http_entry = http_entry->next;
    }

    if (http_entry == NULL) {  //no active http entry with the same request
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

        select_max_fd = MAX(select_max_fd, http_sock_fd);
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
        if ((client->status == DOWNLOADING && client->bytes_written == client->http_entry->data_size) ||
            (client->status == GETTING_FROM_CACHE && client->bytes_written == client->cache_entry->size)) {
            if (client->http_entry != NULL) {
                client->http_entry->clients--;
                client->http_entry = NULL;
            }
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
            client->request_size = 0;
            free(client->request);  client->request = NULL;
        }
        else {
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
        if (ERROR_LOG) perror("write_to_client: Unable to write to client socket");
        client->status = SOCK_ERROR;
        return;
    }
    client->bytes_written += bytes_written;

    check_finished_writing_to_client(client);
}

void init_select_masks() {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(listen_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;

        if (cur_client->status == SOCK_ERROR || cur_client->status == SOCK_DONE || cur_client->status == NON_SOCK_ERROR) {
            remove_client(cur_client);
            cur_client = next;
            continue;
        }

        if (cur_client->http_entry != NULL) {
            if (cur_client->http_entry->status == NON_SOCK_ERROR || cur_client->http_entry->status == SOCK_ERROR) {
                cur_client->status = NON_SOCK_ERROR;
                cur_client->error = cur_client->http_entry->status == SOCK_ERROR ? CONNECTION_ERROR : cur_client->http_entry->error;
                cur_client->http_entry->clients--;
                cur_client->http_entry = NULL;
                cur_client->bytes_written = 0;
            }
            else if (cur_client->http_entry->cache_entry != NULL && cur_client->http_entry->cache_entry->is_full) {
                cur_client->status = GETTING_FROM_CACHE;
                cur_client->cache_entry = cur_client->http_entry->cache_entry;
                cur_client->http_entry->clients--;
                cur_client->http_entry = NULL;
            }
        }
        check_finished_writing_to_client(cur_client);

        FD_SET(cur_client->sock_fd, &readfds);

        if ((cur_client->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) {
            FD_SET(cur_client->sock_fd, &writefds);
        }

        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;

        if (http_check_disconnect(cur_http, &cache)) {
            remove_http(cur_http);
            cur_http = next;
            continue;
        }

        if (cur_http->status != SOCK_ERROR && cur_http->status != SOCK_DONE && cur_http->status != NON_SOCK_ERROR) {
            FD_SET(cur_http->sock_fd, &readfds);
        }
        if (cur_http->status == AWAITING_REQUEST) {
            FD_SET(cur_http->sock_fd, &writefds);
        }

        cur_http = next;
    }
}

void update_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        if (cur_client->status != SOCK_ERROR && cur_client->status != SOCK_DONE && cur_client->status != NON_SOCK_ERROR && FD_ISSET(cur_client->sock_fd, &readfds)) {
            read_data_from_client(cur_client);
        }
        if (((cur_client->status == DOWNLOADING && cur_client->http_entry->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) && FD_ISSET(cur_client->sock_fd, &writefds)) {
            write_to_client(cur_client);
        }
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        if (cur_http->status != SOCK_ERROR && cur_http->status != SOCK_DONE && cur_http->status != NON_SOCK_ERROR && FD_ISSET(cur_http->sock_fd, &readfds)) {
            http_read_data(cur_http, &cache);
        }
        if (cur_http->status == AWAITING_REQUEST && FD_ISSET(cur_http->sock_fd, &writefds)) {
            http_send_request(cur_http);
        }
        cur_http = next;
    }
}

void update_accept() {
    if (FD_ISSET(listen_fd, &readfds)) {
        int client_sock_fd = accept(listen_fd, NULL, NULL);
        if (client_sock_fd == -1) {
            if (ERROR_LOG) perror("update_accept: accept error");
            return;
        }
        create_client(client_sock_fd);
    }
}

int update_stdin() {
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
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
        else if (STR_EQ(buf, "active")) print_active_connections();
    }
    return 0;
}

void proxy_spin() {
    while (TRUE) {
        init_select_masks();

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) perror("proxy_spin: select error");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_connections();
        update_accept();
        if (update_stdin() == -1) break;
    }
}

int parse_port(char *listen_port_str, int *listen_port) {
    if (convert_number(listen_port_str, listen_port) == -1) return -1;
    if (!IS_PORT_VALID(*listen_port)) {
        if (ERROR_LOG) fprintf(stderr, "Invalid port: listen_port=%d\n", *listen_port);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s listen_port\n", argv[0]);
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

    int port;
    if (parse_port(argv[1], &port) == -1) return EXIT_FAILURE;
    if ((listen_fd = open_listen_socket(port)) == -1) return EXIT_FAILURE;
    select_max_fd = MAX(select_max_fd, listen_fd);

    proxy_spin();

    remove_all_connections();
    cache_destroy(&cache);
    close(listen_fd);

    return EXIT_SUCCESS;
}
