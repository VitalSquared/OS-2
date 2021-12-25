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
int current_thread = 0;
int global_thread_count;
cache_t cache;

client_queue_t client_queue = { .head = NULL, .tail = NULL, .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
http_queue_t http_queue = { .head = NULL, .tail = NULL, .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };
client_list_t global_client_list = { .head = NULL, .rwlock = PTHREAD_RWLOCK_INITIALIZER, .size = 0 };
http_list_t global_http_list = { .head = NULL, .rwlock = PTHREAD_RWLOCK_INITIALIZER, .size = 0 };

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
        cur_client = cur_client->global_next;
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
        cur_http = cur_http->global_next;
    }
    unlock_rwlock(&global_http_list.rwlock, "print_active_connections: HTTP");
}

void print_threads_load(thread_param_t *params, int size) {
    for (int i = 0; i < size; i++) {
        printf("- Thread %d: clients=%d, https=%d\n", params[i].index, params[i].client_size, params[i].http_size);
    }
}

int init_client_select_masks(client_list_t *client_list, fd_set *readfds, fd_set *writefds) {
    int select_max_fd = -1;

    client_t *client = client_list->head;
    while (client != NULL) {
        client_t *next = client->next;

        if (IS_ERROR_OR_DONE_STATUS(client->status)) {
            remove_client(client, client_list, &global_client_list);
            client = next;
            continue;
        }

        client_update_http_info(client);
        check_finished_writing_to_client(client);

        if (client->http_entry != NULL) {
            FD_SET(client->http_entry->client_pipe_fd, readfds);
            select_max_fd = MAX(select_max_fd, client->http_entry->client_pipe_fd);
        }

        FD_SET(client->sock_fd, readfds);
        select_max_fd = MAX(client->sock_fd, select_max_fd);

        if (client->status == DOWNLOADING) {
            read_lock_rwlock(&client->http_entry->rwlock, "client_worker: DOWNLOADING FD_SET");
            if (client->bytes_written < client->http_entry->data_size) {
                FD_SET(client->sock_fd, writefds);
            }
            unlock_rwlock(&client->http_entry->rwlock, "client_worker: DOWNLOADING FD_SET");
        }
        else if (client->status == GETTING_FROM_CACHE) {
            read_lock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE FD_SET");
            if (client->bytes_written < client->cache_entry->size) {
                FD_SET(client->sock_fd, writefds);
            }
            unlock_rwlock(&client->cache_entry->rwlock, "client_worker: CACHE FD_SET");
        }

        client = next;
    }

    return select_max_fd;
}

void update_client_connections(client_list_t *client_list, fd_set *readfds, fd_set *writefds) {
    client_t *client = client_list->head;
    while (client != NULL) {
        client_t *next = client->next;

        char buf[1] = { 1 };
        if (client->http_entry != NULL && FD_ISSET(client->http_entry->client_pipe_fd, readfds)) {
            read(client->http_entry->client_pipe_fd, buf, 1);
        }

        if (!IS_ERROR_OR_DONE_STATUS(client->status) && FD_ISSET(client->sock_fd, readfds)) {
            client_read_data(client, &global_http_list, &http_queue, &cache);
        }
        if (FD_ISSET(client->sock_fd, writefds)) {
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

int init_http_select_masks(http_list_t *http_list, fd_set *readfds, fd_set *writefds) {
    int select_max_fd = -1;

    http_t *http = http_list->head;
    while (http != NULL) {
        http_t *next = http->next;

        if (http_check_disconnect(http)) {
            remove_http(http, http_list, &global_http_list, &cache);
            http = next;
            continue;
        }

        FD_SET(http->http_pipe_fd, readfds);
        select_max_fd = MAX(select_max_fd, http->http_pipe_fd);

        if (!IS_ERROR_OR_DONE_STATUS(http->status)) {
            FD_SET(http->sock_fd, readfds);
            select_max_fd = MAX(http->sock_fd, select_max_fd);
        }
        if (http->status == AWAITING_REQUEST) {
            FD_SET(http->sock_fd, writefds);
            select_max_fd = MAX(http->sock_fd, select_max_fd);
        }

        http = next;
    }

    return select_max_fd;
}

void update_http_connections(http_list_t *http_list, fd_set *readfds, fd_set *writefds) {
    http_t *http = http_list->head;
    while (http != NULL) {
        http_t *next = http->next;
        char buf[1] = { 1 };
        if (FD_ISSET(http->http_pipe_fd, readfds)) {
            read(http->http_pipe_fd, buf, 1);
        }
        if (!IS_ERROR_OR_DONE_STATUS(http->status) && FD_ISSET(http->sock_fd, readfds)) {
            http_read_data(http, &cache);
        }
        if (http->status == AWAITING_REQUEST && FD_ISSET(http->sock_fd, writefds)) {
            http_send_request(http);
        }
        http = next;
    }
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

void *connection_worker(void *_param) {
    thread_param_t *param = (thread_param_t *)_param;
    if (param == NULL) {
        fprintf(stderr, "connection_worker: param was NULL\n");
        return NULL;
    }

    client_list_t client_list = { .head = NULL, .size = 0 };
    http_list_t http_list = { .head = NULL, .size = 0 };
    fd_set readfds, writefds;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    pthread_cleanup_push(http_cancel_handler, &http_list);
    pthread_cleanup_push(client_cancel_handler, &client_list);
    while (TRUE) {
        param->http_size = http_list.size;
        param->client_size = client_list.size;

        pthread_mutex_lock(&client_queue.mutex);
        client_queue.max_num = MAX(client_queue.max_num, http_list.size + client_list.size);
        pthread_mutex_unlock(&client_queue.mutex);

        client_t *new_client = client_dequeue(&client_queue, http_list.size + client_list.size, param->index, &current_thread, global_thread_count);
        if (new_client != NULL) {
            client_add_to_list(new_client, &client_list);
            client_add_to_global_list(new_client, &global_client_list);
        }

        pthread_mutex_lock(&http_queue.mutex);
        http_queue.max_num = MAX(http_queue.max_num, http_list.size + client_list.size);
        pthread_mutex_unlock(&http_queue.mutex);

        http_t *new_http = http_dequeue(&http_queue, http_list.size + client_list.size, param->index, &current_thread, global_thread_count);
        if (new_http != NULL) {
            http_add_to_list(new_http, &http_list);
            http_add_to_global_list(new_http, &global_http_list);
        }

        int select_max_fd = -1;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        int select_max_fd1 = init_client_select_masks(&client_list, &readfds, &writefds);
        int select_max_fd2 = init_http_select_masks(&http_list, &readfds, &writefds);
        select_max_fd = MAX(select_max_fd, select_max_fd1);
        select_max_fd = MAX(select_max_fd, select_max_fd2);

        FD_SET(param->new_connection_pipe_fd, &readfds);
        select_max_fd = MAX(select_max_fd, param->new_connection_pipe_fd);

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) fprintf(stderr, "connection_worker: select error\n");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_client_connections(&client_list, &readfds, &writefds);
        update_http_connections(&http_list, &readfds, &writefds);

        if (FD_ISSET(param->new_connection_pipe_fd, &readfds)) {
            char buf[1];
            read(param->new_connection_pipe_fd, buf, 1);
        }
    }
    pthread_cleanup_pop(TRUE);
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

int update_stdin(fd_set *readfds, thread_param_t *params, int size) {
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
        else if (STR_EQ(buf, "load")) print_threads_load(params, size);
    }
    return 0;
}

void proxy_spin(thread_param_t *params, int size) {
    fd_set readfds;

    while (TRUE) {
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int num_fds_ready = select(listen_fd + 1, &readfds, NULL, NULL, NULL);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) perror("proxy_spin: select error");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_accept(&readfds);
        if (update_stdin(&readfds, params, size) == -1) break;
    }
}

int parse_args(char *listen_port_str, int *listen_port, char *pool_size_str, int *pool_size) {
    if (convert_number(listen_port_str, listen_port) == -1 || convert_number(pool_size_str, pool_size) == -1) {
        return -1;
    }
    if (!IS_PORT_VALID(*listen_port) || !IS_POOL_SIZE_VALID(*pool_size)) {
        if (ERROR_LOG) fprintf(stderr, "Invalid port: listen_port=%d, pool_size=%d\n", *listen_port, *pool_size);
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
    close(client_queue.wakeup_pipe_fd);
    close(listen_fd);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s listen_port pool_size\n", argv[0]);
        return EXIT_SUCCESS;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("main: signal error");
        return EXIT_FAILURE;
    }

    int fildes[2];
    if (open_wakeup_pipe(&fildes[0], &fildes[1]) == -1) {
        return EXIT_FAILURE;
    }
    if (cache_init(&cache) != 0) {
        fprintf(stderr, "Unable to init cache\n");
        return EXIT_FAILURE;
    }

    int port, pool_size;
    if (parse_args(argv[1], &port, argv[2], &pool_size) == -1) return EXIT_FAILURE;
    if ((listen_fd = open_listen_socket(port)) == -1) return EXIT_FAILURE;
    atexit(cleanup);

    client_queue.wakeup_pipe_fd = fildes[0];
    client_queue.max_num = 0;
    http_queue.wakeup_pipe_fd = fildes[0];
    http_queue.max_num = 0;

    int err_code, threads_created = 0;
    pthread_t threads[pool_size];
    thread_param_t param[pool_size];
    pthread_mutex_lock(&client_queue.mutex);
    for (int i = 0; i < pool_size; i++) {
        param[i].index = i;
        param[i].new_connection_pipe_fd = fildes[1];
        param[i].http_size = 0;
        param[i].client_size = 0;

        err_code = pthread_create(&threads[i], NULL, connection_worker, &param[i]);
        if (err_code != 0) {
            print_error("Unable to create pool thread\n", err_code);
            break;
        }
        pthread_detach(threads[i]);
        threads_created++;
    }
    fprintf(stderr, "Created %d out of %d pool threads!\n", threads_created, pool_size);
    if (threads_created == 0) return EXIT_FAILURE;
    global_thread_count = threads_created;
    pthread_mutex_unlock(&client_queue.mutex);

    proxy_spin(param, threads_created);

    for (int i = 0; i < threads_created; i++) pthread_cancel(threads[i]);
    remove_all_queued_connections();
    pthread_exit(NULL);
}
