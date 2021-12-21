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
#include "client.h"
#include "cache.h"
#include "list_queue.h"

int listen_fd;
cache_t cache;

client_queue_t client_queue = { .head = NULL, .tail = NULL, .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
http_queue_t http_queue = { .head = NULL, .tail = NULL, .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
client_list_t global_client_list = { .head = NULL, .rwlock = PTHREAD_RWLOCK_INITIALIZER };
http_list_t global_http_list = { .head = NULL, .rwlock = PTHREAD_RWLOCK_INITIALIZER };

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
        http_destroy(http, &cache);
        free(http);
        http = next;
    }
}

void print_active_connections() {
    read_lock_rwlock(&global_client_list.rwlock, "print_active_connections: CLIENT");
    client_t *cur_client = global_client_list.head;
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
    unlock_rwlock(&global_client_list.rwlock, "print_active_connections: CLIENT");

    printf("\n");

    read_lock_rwlock(&global_http_list.rwlock, "print_active_connections: HTTP");
    http_t *cur_http = global_http_list.head;
    while (cur_http != NULL) {
        printf("[http %d] status=%d, code=%d, clients=%d, is_response_complete=%d, response_type=%d\n", cur_http->sock_fd, cur_http->status, cur_http->code, cur_http->clients, cur_http->is_response_complete, cur_http->response_type);
        if (cur_http->cache_entry != NULL) {
            printf("- cache=%s %s, size=%zd\n", cur_http->cache_entry->host, cur_http->cache_entry->path, cur_http->cache_entry->size);
        }
        cur_http = cur_http->next;
    }
    unlock_rwlock(&global_http_list.rwlock, "print_active_connections: HTTP");
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
        remove_client(client, client_list, &global_client_list);
        client = next;
    }

    return NULL;
}

void *client_worker(void *param) {
    client_list_t client_list = { .head = NULL };
    fd_set readfds, writefds;
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_cleanup_push(client_cancel_handler, &client_list);
    while (TRUE) {
        client_t *new_client = client_dequeue(&client_queue, client_list.head == NULL);
        if (new_client != NULL) {
            client_add_to_list(new_client, &client_list);
            client_add_to_global_list(new_client, &global_client_list);
        }

        int select_max_fd = 0;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        client_t *client = client_list.head;
        while (client != NULL) {
            client_t *next = client->next;

            if (IS_ERROR_OR_DONE_STATUS(client->status)) {
                break;
            }

            client_update_http_info(client);
            check_finished_writing_to_client(client);

            FD_SET(client->sock_fd, &readfds);
            if (client->status == DOWNLOADING) {
                read_lock_rwlock(&client->http_entry->rwlock, "client_worker: DOWNLOADING FD_SET");
                if (client->bytes_written < client->http_entry->data_size) {
                    FD_SET(client->sock_fd, &writefds);
                }
                unlock_rwlock(&client->http_entry->rwlock, "client_worker: DOWNLOADING FD_SET");
            }
            else if (client->status == GETTING_FROM_CACHE) {
                read_lock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE FD_SET");
                if (client->bytes_written < client->cache_entry->size) {
                    FD_SET(client->sock_fd, &writefds);
                }
                unlock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE FD_SET");
            }

            select_max_fd = MAX(client->sock_fd, select_max_fd);
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

            if (!IS_ERROR_OR_DONE_STATUS(client->status) && FD_ISSET(client->sock_fd, &readfds)) {
                client_read_data(client, &global_http_list, &http_queue, &cache);
            }
            if (FD_ISSET(client->sock_fd, &writefds)) {
                ssize_t http_data_size = 0;
                int http_status;
                if (client->http_entry != NULL) {
                    read_lock_rwlock(&client->http_entry->rwlock, "client_worker: HTTP POST select");
                    http_data_size = client->http_entry->data_size;
                    http_status = client->http_entry->status;
                    unlock_rwlock(&client->http_entry->rwlock, "client_worker: HTTP POST select");
                }

                ssize_t cache_data_size = 0;
                if (client->cache_entry != NULL) {
                    read_lock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE POST select");
                    cache_data_size = client->cache_entry->size;
                    unlock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE POST select");
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
        remove_http(http, http_list, &global_http_list, &cache);
        http = next;
    }

    return NULL;
}

void *http_worker(void *param) {
    http_list_t http_list = { .head = NULL };
    fd_set readfds, writefds;
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_cleanup_push(http_cancel_handler, &http_list);
    while (TRUE) {
        http_t *new_http = http_dequeue(&http_queue, http_list.head == NULL);
        if (new_http != NULL) {
            http_add_to_list(new_http, &http_list);
            http_add_to_global_list(new_http, &global_http_list);
        }

        int select_max_fd = 0;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        http_t *http = http_list.head;
        while (http != NULL) {
            http_t *next = http->next;

            if (http_check_disconnect(http)) {
                break;
            }

            if (!IS_ERROR_OR_DONE_STATUS(http->status)) {
                FD_SET(http->sock_fd, &readfds);
            }
            if (http->status == AWAITING_REQUEST) {
                FD_SET(http->sock_fd, &writefds);
            }

            select_max_fd = MAX(http->sock_fd, select_max_fd);
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
            if (!IS_ERROR_OR_DONE_STATUS(http->status) && FD_ISSET(http->sock_fd, &readfds)) {
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
        create_client(client_sock_fd, &client_queue);
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

void proxy_spin() {
    fd_set readfds;
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };

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
    pthread_mutex_destroy(&client_queue.mutex);
    pthread_cond_destroy(&client_queue.cond);
    pthread_mutex_destroy(&http_queue.mutex);
    pthread_cond_destroy(&http_queue.cond);
    pthread_rwlock_destroy(&global_client_list.rwlock);
    pthread_rwlock_destroy(&global_http_list.rwlock);
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
