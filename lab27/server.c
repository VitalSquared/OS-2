#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG 1
#define BUF_SIZE 4096
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define IS_PORT_VALID(port) (0 < (port) && (port) <= 0xFFFF)

#define SOCK_OK (0)
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)

#define TRUE 1
#define FALSE 0

typedef struct list_node {
    int client_sock_fd, remote_sock_fd;

    ssize_t client_size, client_offset, client_status;
    char client_read_data[BUF_SIZE];

    ssize_t remote_size, remote_offset, remote_status;
    char remote_read_data[BUF_SIZE];

    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct list {
    list_node_t *head;
} list_t;

int listen_fd;
int should_work = TRUE;
int select_max_fd = 0;
fd_set readfds, writefds;
list_t clients_list = { .head = NULL };

void sigint_handler(int sig) {
    if (sig == SIGINT) {
        should_work = FALSE;
    }
}

void list_add(list_node_t *node) {
    node->prev = NULL;
    node->next = clients_list.head;
    clients_list.head = node;
    if (node->next != NULL) {
        node->next->prev = node;
    }
}

void list_remove(list_node_t *node) {
    if (node == NULL) {
        return;
    }
    if (node == clients_list.head) {
        clients_list.head = node->next;
        if (clients_list.head != NULL) clients_list.head->prev = NULL;
    }
    else {
        node->prev->next = node->next;
        if (node->next != NULL) node->next->prev = node->prev;
    }
}

list_node_t *create_connection(int client_sock_fd, struct sockaddr_in *remote_addr) {
    list_node_t *new_connection = (list_node_t *)calloc(1, sizeof(list_node_t));
    if (new_connection == NULL) {
        perror("Unable to allocate memory for connection");
        close(client_sock_fd);
        return NULL;
    }
    new_connection->client_sock_fd = client_sock_fd;

    if ((new_connection->remote_sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("unable to create remote socket");
        close(client_sock_fd);
        free(new_connection);
        return NULL;
    }
    if (connect(new_connection->remote_sock_fd, (struct sockaddr *)remote_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("unable to connect to remote socket");
        close(new_connection->client_sock_fd);
        close(new_connection->remote_sock_fd);
        free(new_connection);
        return NULL;
    }

    new_connection->client_status = SOCK_OK;
    new_connection->remote_status = SOCK_OK;

    if (LOG) printf("New client %d connected\n", client_sock_fd);

    select_max_fd = MAX(select_max_fd, new_connection->client_sock_fd);
    select_max_fd = MAX(select_max_fd, new_connection->remote_sock_fd);
    
    return new_connection;
}

void drop_connection(list_node_t *connection) {
    list_remove(connection);

    if (LOG) printf("Client %d disconnected\n", connection->client_sock_fd);

    close(connection->client_sock_fd);
    close(connection->remote_sock_fd);

    free(connection);
}

void init_select_masks() {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(listen_fd, &readfds);

    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        list_node_t *next = cur->next;

        if (!(cur->client_size == 0 && cur->client_status == SOCK_OK && cur->remote_size == 0 && cur->remote_status == SOCK_OK) &&
            (cur->client_status != SOCK_OK || cur->client_size == 0) &&
            (cur->remote_status != SOCK_OK || cur->remote_size == 0))
        {
            drop_connection(cur);
            cur = next;
            continue;
        }

        if (cur->client_size == 0 && cur->client_status == SOCK_OK) {
            FD_SET(cur->client_sock_fd, &readfds);
            cur->client_offset = 0;
        }
        if (cur->remote_size == 0 && cur->remote_status == SOCK_OK) {
            FD_SET(cur->remote_sock_fd, &readfds);
            cur->remote_offset = 0;
        }
        if (cur->client_size > 0 && cur->remote_status == SOCK_OK) {
            FD_SET(cur->remote_sock_fd, &writefds);
        }
        if (cur->remote_size > 0 && cur->client_status == SOCK_OK) {
            FD_SET(cur->client_sock_fd, &writefds);
        }

        cur = next;
    }
}

void update_connections() {
    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        if (cur->client_size == 0 && cur->client_status == SOCK_OK && FD_ISSET(cur->client_sock_fd, &readfds)) {
            cur->client_size = read(cur->client_sock_fd, cur->client_read_data, BUF_SIZE);

            if (cur->client_size == -1) {
                perror("Unable to read from client socket");
                cur->client_status = SOCK_ERROR;
            }
            if (cur->client_size == 0) {
                cur->client_status = SOCK_DONE;
            }
        }
        if (cur->remote_size == 0 && cur->remote_status == SOCK_OK && FD_ISSET(cur->remote_sock_fd, &readfds)) {
            cur->remote_size = read(cur->remote_sock_fd, cur->remote_read_data, BUF_SIZE);

            if (cur->remote_size == -1) {
                perror("Unable to read from remote socket");
                cur->remote_status = SOCK_ERROR;
            }
            if (cur->remote_size == 0) {
                cur->remote_status = SOCK_DONE;
            }
        }
        if (cur->client_size > 0 && cur->remote_status == SOCK_OK && FD_ISSET(cur->remote_sock_fd, &writefds)) {
            ssize_t bytes_written = write(cur->remote_sock_fd, cur->client_read_data + cur->client_offset, cur->client_size);

            if (bytes_written == -1) {
                perror("Unable to write to remote socket");
                cur->remote_status = SOCK_ERROR;
            }
            else {
                cur->client_size -= bytes_written;
                cur->client_offset += bytes_written;
            }
        }
        if (cur->remote_size > 0 && cur->client_status == SOCK_OK && FD_SET(cur->client_sock_fd, &writefds)) {
            ssize_t bytes_written = write(cur->client_sock_fd, cur->remote_read_data + cur->remote_offset, cur->remote_size);

            if (bytes_written == -1) {
                perror("Unable to write to client socket");
                cur->client_status = SOCK_ERROR;
            }
            else {
                cur->remote_size -= bytes_written;
                cur->remote_offset += bytes_written;
            }
        }

        cur = cur->next;
    }
}

int open_listen_socket(int port) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket error");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("bind error");
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, SOMAXCONN) == -1) {
        perror("listen error");
        close(sock_fd);
        return -1;
    }

    select_max_fd = MAX(select_max_fd, sock_fd);
    return sock_fd;
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

int init_remote_addr(char *hostname, int remote_port, struct sockaddr_in *remote_addr) {
    int error_code;
    struct hostent *remote_host = getipnodebyname(hostname, AF_INET, 0, &error_code);
    if (remote_host == NULL) {
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(error_code));
        return -1;
    }

    memset(remote_addr, 0, sizeof(struct sockaddr_in));
    remote_addr->sin_family = AF_INET;
    memcpy(&remote_addr->sin_addr.s_addr, remote_host->h_addr_list[0], sizeof(struct in_addr));
    remote_addr->sin_port = htons(remote_port);
    freehostent(remote_host);

    return 0;
}

void disconnect_all_clients() {
    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        list_node_t *next = cur->next;
        drop_connection(cur);
        cur = next;
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s listen_port remote_host remote_port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Unable to set signal handling");
        return EXIT_FAILURE;
    }

    int listen_port = atoi(argv[1]), remote_port = atoi(argv[3]);
    char *hostname = argv[2];

    if (!IS_PORT_VALID(listen_port) || !IS_PORT_VALID(remote_port)) {
        fprintf(stderr, "Invalid port(s): listen_port=%d, remote_port=%d\n", listen_port, remote_port);
        return EXIT_FAILURE;
    }

    struct sockaddr_in remote_addr;
    if (init_remote_addr(hostname, remote_port, &remote_addr) == -1) {
        return EXIT_FAILURE;
    }

    if ((listen_fd = open_listen_socket(listen_port)) == -1) {
        return EXIT_FAILURE;
    }

    while (should_work) {
        init_select_masks();

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (num_fds_ready == -1) {
            perror("select error");
            break;
        }
        if (num_fds_ready == 0) {
            continue;
        }

        update_connections();

        if (FD_ISSET(listen_fd, &readfds)) {
            int client_sock_fd = accept(listen_fd, NULL, NULL);
            if (client_sock_fd == -1) {
                perror("accept error");
                break;
            }
            list_node_t *node = create_connection(client_sock_fd, &remote_addr);
            if (node != NULL) list_add(node);
        }
    }

    disconnect_all_clients();
    return EXIT_SUCCESS;
}
