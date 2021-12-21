/*
 * Proxy, created by Vitaly Spirin, NSU, group 19203
 * This proxy uses picohttpparser: https://github.com/h2o/picohttpparser
 */

#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "http.h"
#include "client.h"
#include "cache.h"
#include "types.h"
#include "list.h"

cache_t cache;
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

void remove_all_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        remove_client(cur_client, &client_list);
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        remove_http(cur_http, &http_list, &cache);
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

void init_select_masks(fd_set *readfds, fd_set *writefds, int *select_max_fd) {
    FD_ZERO(readfds);
    FD_ZERO(writefds);

    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;

        if (IS_ERROR_OR_DONE_STATUS(cur_client->status)) {
            remove_client(cur_client, &client_list);
            cur_client = next;
            continue;
        }

        client_update_http_info(cur_client);
        check_finished_writing_to_client(cur_client);

        FD_SET(cur_client->sock_fd, readfds);
        if ((cur_client->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) {
            FD_SET(cur_client->sock_fd, writefds);
        }

        *select_max_fd = MAX(*select_max_fd, cur_client->sock_fd);
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;

        if (http_check_disconnect(cur_http)) {
            remove_http(cur_http, &http_list, &cache);
            cur_http = next;
            continue;
        }

        if (!IS_ERROR_OR_DONE_STATUS(cur_http->status)) {
            FD_SET(cur_http->sock_fd, readfds);
        }
        if (cur_http->status == AWAITING_REQUEST) {
            FD_SET(cur_http->sock_fd, writefds);
        }

        *select_max_fd = MAX(*select_max_fd, cur_http->sock_fd);
        cur_http = next;
    }
}

void update_connections(fd_set *readfds, fd_set *writefds) {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        if (!IS_ERROR_OR_DONE_STATUS(cur_client->status) && FD_ISSET(cur_client->sock_fd, readfds)) {
            client_read_data(cur_client, &http_list, &cache);
        }
        if (((cur_client->status == DOWNLOADING && cur_client->http_entry->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) && FD_ISSET(cur_client->sock_fd,  writefds)) {
            write_to_client(cur_client);
        }
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        if (!IS_ERROR_OR_DONE_STATUS(cur_http->status) && FD_ISSET(cur_http->sock_fd, readfds)) {
            http_read_data(cur_http, &cache);
        }
        if (cur_http->status == AWAITING_REQUEST && FD_ISSET(cur_http->sock_fd, writefds)) {
            http_send_request(cur_http);
        }
        cur_http = next;
    }
}

void update_accept(fd_set *readfds, int listen_fd) {
    if (FD_ISSET(listen_fd, readfds)) {
        int client_sock_fd = accept(listen_fd, NULL, NULL);
        if (client_sock_fd == -1) {
            if (ERROR_LOG) perror("update_accept: accept error");
            return;
        }
        create_client(client_sock_fd, &client_list);
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
        else if (STR_EQ(buf, "active")) print_active_connections();
    }
    return 0;
}

void proxy_spin(int listen_fd) {
    fd_set readfds, writefds;
    int select_max_fd;

    while (TRUE) {
        select_max_fd = MAX(STDIN_FILENO, listen_fd);
        init_select_masks(&readfds, &writefds, &select_max_fd);
        FD_SET(listen_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) perror("proxy_spin: select error");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_connections(&readfds, &writefds);
        update_accept(&readfds, listen_fd);
        if (update_stdin(&readfds) == -1) break;
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

    int listen_fd = open_listen_socket(port);
    if (listen_fd == -1) return EXIT_FAILURE;

    proxy_spin(listen_fd);

    remove_all_connections();
    cache_destroy(&cache);
    close(listen_fd);

    return EXIT_SUCCESS;
}
