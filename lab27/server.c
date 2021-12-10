/*
 * This server listens on port 'listen_port'.
 * For every accepted client, server creates connection with remote ('remote_host' + 'remote_port').
 * All data received from client is being sent to remote.
 * And every data received from remote is being sent to client.
 *
 * Created by Vitaly Spirin, NSU, group 19203
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

#define LOG 1
#define BUF_SIZE 4096
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define IS_PORT_VALID(port) (0 < (port) && (port) <= 0xFFFF)

#define IS_CLIENT_INADEQUATE(client)    \
            (                           \
                !((client)->client_size == 0 && (client)->client_status == SOCK_OK && (client)->remote_size == 0 && (client)->remote_status == SOCK_OK) &&  \
                ((client)->client_status != SOCK_OK || (client)->client_size == 0) &&                                                                       \
                ((client)->remote_status != SOCK_OK || (client)->remote_size == 0)                                                                          \
            )  

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

    struct list_node *prev, *next;
} list_node_t;

typedef struct list {
    list_node_t *head;
} list_t;

int listen_fd;
int should_work = TRUE;
int select_max_fd = 0;
fd_set readfds, writefds;
struct sockaddr_in remote_addr;
list_t clients_list = { .head = NULL };

void sigint_handler(int sig) {
    if (sig == SIGINT) {
        printf(" SIGINT caught\n");
        should_work = FALSE;
    }
}

int init_signals() {
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Unable to set signal handling");
        return -1;
    }
    return 0;
}

void list_add(list_node_t *node) {
    node->prev = NULL;
    node->next = clients_list.head;
    clients_list.head = node;
    if (node->next != NULL) node->next->prev = node;
}

void list_remove(list_node_t *node) {
    if (node == clients_list.head) {
        clients_list.head = node->next;
        if (clients_list.head != NULL) clients_list.head->prev = NULL;
    }
    else {
        node->prev->next = node->next;
        if (node->next != NULL) node->next->prev = node->prev;
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

int init_remote_addr(char *hostname, int remote_port) {
    int error_code;
    struct hostent *remote_host = getipnodebyname(hostname, AF_INET, 0, &error_code);
    if (remote_host == NULL) {
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(error_code));
        return -1;
    }

    memset(&remote_addr, 0, sizeof(struct sockaddr_in));

    remote_addr.sin_family = AF_INET;
    memcpy(&remote_addr.sin_addr.s_addr, remote_host->h_addr_list[0], sizeof(struct in_addr));
    remote_addr.sin_port = htons(remote_port);

    freehostent(remote_host);

    return 0;
}

int open_remote_socket() {
    int sock_fd;
    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("unable to create remote socket");
        return -1;
    }
    if (connect(sock_fd, (struct sockaddr *) &remote_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("unable to connect to remote socket");
        close(sock_fd);
        return -1;
    }
    return sock_fd;
}

list_node_t *create_client(int client_sock_fd) {
    list_node_t *client = (list_node_t *)calloc(1, sizeof(list_node_t));
    if (client == NULL) {
        perror("Unable to allocate memory for client");
        close(client_sock_fd);
        return NULL;
    }
    client->client_sock_fd = client_sock_fd;

    if ((client->remote_sock_fd = open_remote_socket()) == -1) {
        close(client_sock_fd);
        free(client);
        return NULL;
    }

    client->client_status = SOCK_OK;
    client->remote_status = SOCK_OK;

    if (LOG) printf("New client %d connected\n", client_sock_fd);

    select_max_fd = MAX(select_max_fd, client->client_sock_fd);
    select_max_fd = MAX(select_max_fd, client->remote_sock_fd);
    
    return client;
}

void remove_client(list_node_t *client) {
    list_remove(client);

    if (LOG) printf("Client %d disconnected\n", client->client_sock_fd);

    close(client->client_sock_fd);
    close(client->remote_sock_fd);

    free(client);
}

void remove_all_clients() {
    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        list_node_t *next = cur->next;
        remove_client(cur);
        cur = next;
    }
}

void init_select_masks() {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(listen_fd, &readfds);

    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        list_node_t *next = cur->next;

        if (IS_CLIENT_INADEQUATE(cur)) {
            remove_client(cur);
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

void update_read_from_client(list_node_t *client) {
    if (client->client_size == 0 && client->client_status == SOCK_OK && FD_ISSET(client->client_sock_fd, &readfds)) {
        client->client_size = read(client->client_sock_fd, client->client_read_data, BUF_SIZE);

        if (client->client_size == -1) {
            perror("Unable to read from client socket");
            client->client_status = SOCK_ERROR;
        }
        if (client->client_size == 0) {
            client->client_status = SOCK_DONE;
        }
    }
}

void update_read_from_remote(list_node_t *client) {
    if (client->remote_size == 0 && client->remote_status == SOCK_OK && FD_ISSET(client->remote_sock_fd, &readfds)) {
        client->remote_size = read(client->remote_sock_fd, client->remote_read_data, BUF_SIZE);

        if (client->remote_size == -1) {
            perror("Unable to read from remote socket");
            client->remote_status = SOCK_ERROR;
        }
        if (client->remote_size == 0) {
            client->remote_status = SOCK_DONE;
        }
    }
}

void update_write_to_remote(list_node_t *client) {
    if (client->client_size > 0 && client->remote_status == SOCK_OK && FD_ISSET(client->remote_sock_fd, &writefds)) {
        ssize_t bytes_written = write(client->remote_sock_fd, client->client_read_data + client->client_offset, client->client_size);

        if (bytes_written == -1) {
            perror("Unable to write to remote socket");
            client->remote_status = SOCK_ERROR;
            return;
        }

        client->client_size -= bytes_written;
        client->client_offset += bytes_written;
    }
}

void update_write_to_client(list_node_t *client) {
    if (client->remote_size > 0 && client->client_status == SOCK_OK && FD_SET(client->client_sock_fd, &writefds)) {
        ssize_t bytes_written = write(client->client_sock_fd, client->remote_read_data + client->remote_offset, client->remote_size);

        if (bytes_written == -1) {
            perror("Unable to write to client socket");
            client->client_status = SOCK_ERROR;
            return;
        }

        client->remote_size -= bytes_written;
        client->remote_offset += bytes_written;
    }
}

void update_clients() {
    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        update_read_from_client(cur);
        update_read_from_remote(cur);
        update_write_to_remote(cur);
        update_write_to_client(cur);
        cur = cur->next;
    }
}

int check_accept() {
    if (FD_ISSET(listen_fd, &readfds)) {
        int client_sock_fd = accept(listen_fd, NULL, NULL);
        if (client_sock_fd == -1) {
            perror("accept error");
            return -1;
        }
        list_node_t *node = create_client(client_sock_fd);
        if (node != NULL) list_add(node);
    }
    return 0;
}

void server_spin() {
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

        update_clients();
        if (check_accept() == -1) break;
    }
}

int convert_number(char *str, int *number) {
    errno = 0;
    char *endptr = "";
    long num = strtol(str, &endptr, 10);

    if (errno != 0) {
        perror("Can't convert given number");
        return -1;
    }
    if (strcmp(endptr, "") != 0) {
        fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }

    *number = (int)num;
    return 0;
}

int parse_ports(char *listen_port_str, char *remote_port_str, int *listen_port, int *remote_port) {
    if (convert_number(listen_port_str, listen_port) == -1 || convert_number(remote_port_str, remote_port) == -1) {
        return -1;
    }

    if (!IS_PORT_VALID(*listen_port) || !IS_PORT_VALID(*remote_port)) {
        fprintf(stderr, "Invalid port(s): listen_port=%d, remote_port=%d\n", *listen_port, *remote_port);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s listen_port remote_host remote_port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (init_signals() == -1) {
        return EXIT_FAILURE;
    }

    char *hostname = argv[2];
    int listen_port, remote_port;
    if (parse_ports(argv[1], argv[3], &listen_port, &remote_port) == -1) {
        return EXIT_FAILURE;
    }

    if (init_remote_addr(hostname, remote_port) == -1) {
        return EXIT_FAILURE;
    }

    if ((listen_fd = open_listen_socket(listen_port)) == -1) {
        return EXIT_FAILURE;
    }

    server_spin();

    remove_all_clients();
    return EXIT_SUCCESS;
}
