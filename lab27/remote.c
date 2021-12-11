/*
 * This program creates a remote hub.
 * When client connects, it starts sending responses for every message from client.
 * Data is being read from files line-by-line.
 * When end of file is reached, we wait for client to disconnect.
 * Use 'filegen.c' to generate files. You have to enter the same prefix you used in 'filegen'. Keep in mind current working directory.
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
#include <fcntl.h>
#include <sys/stat.h>

#define SOCK_OK (0)
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)

#define FILE_OK (0)
#define FILE_ERROR (-1)
#define FILE_DONE (-2)

#define BUF_SIZE 512
#define FILE_READ_BUF_SIZE 64
#define MAX_NUM_OF_FILES 512
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define IS_PORT_VALID(port) (0 < (port) && (port) <= 0xFFFF)
#define IS_NUM_OF_CLIENTS_VALID(n) (0 < (n) && (n) <= 512)

#define IS_CLIENT_INADEQUATE(client) ((client)->sock_status != SOCK_OK || (client)->file_status == FILE_ERROR)

#define TRUE 1
#define FALSE 0

const char *prefix = "remote_files/remote";
const char file_names[MAX_NUM_OF_FILES][BUF_SIZE];

typedef struct list_node {
    int sock_fd, file_fd;
    int sock_status, file_status;

    char buf[BUF_SIZE];
    ssize_t buf_size;

    int received_response;

    struct list_node *prev, *next;
} list_node_t;

typedef struct list {
    list_node_t *head;
} list_t;

int listen_fd;
int select_max_fd = 0;
int num_of_files = -1;
int file_index_offset = 0;
int should_work = TRUE;
fd_set readfds, writefds;
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

void calc_max_file_index() {
    struct stat statbuf;
    int i;

    for (i = 0; i < MAX_NUM_OF_FILES; i++) {
        char name[BUF_SIZE] = { 0 };
        sprintf(name, "%s_%d.txt", prefix, i);
        if (stat(name, &statbuf) == -1) {
            break;
        }
        memcpy((void *)file_names[i], name, strlen(name) + 1);
    }

    num_of_files = i;
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

list_node_t *create_client(int sock_fd, int file_index) {
    list_node_t *client = (list_node_t *)calloc(1, sizeof(list_node_t));
    if (client == NULL) {
        perror("Unable to allocate memory for client");
        return NULL;
    }

    int file_fd = open(file_names[file_index], O_RDONLY);
    if (file_fd == -1) {
        perror(file_names[file_index]);
        close(sock_fd);
        free(client);
        return NULL;
    }

    client->sock_fd = sock_fd;
    client->file_fd = file_fd;
    client->sock_status = SOCK_OK;
    client->file_status = FILE_OK;
    client->received_response = FALSE;

    printf("New client %d connected\n", client->sock_fd);

    select_max_fd = MAX(select_max_fd, client->sock_fd);
    select_max_fd = MAX(select_max_fd, client->file_fd);

    return client;
}

void remove_client(list_node_t *client) {
    list_remove(client);

    printf("Client %d disconnected\n", client->sock_fd);

    close(client->sock_fd);
    close(client->file_fd);

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

ssize_t read_line(int fd, char *buf, size_t buf_size) {
    ssize_t bytes_read = read(fd, buf, buf_size);
    if (bytes_read <= 0) {
        return bytes_read;
    }

    size_t new_line_pos = -1;
    for (size_t i = 0; i < bytes_read; i++) {
        if (buf[i] == '\n') {
            new_line_pos = i;
            break;
        }
    }
    if (new_line_pos == -1) {
        return bytes_read;
    }

    if (lseek(fd, -(bytes_read - new_line_pos - 1), SEEK_CUR) == -1) {
        return -1;
    }

    return (ssize_t)(new_line_pos);
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

        if (cur->sock_status == SOCK_OK) {
            FD_SET(cur->sock_fd, &readfds);
        }
        if (cur->buf_size == 0 && cur->received_response && cur->file_status == FILE_OK) {
            FD_SET(cur->file_fd, &readfds);
        }
        if (cur->buf_size > 0 && cur->sock_status == SOCK_OK) {
            FD_SET(cur->sock_fd, &writefds);
        }

        cur = next;
    }
}

void update_read_from_client(list_node_t *client) {
    if (client->sock_status == SOCK_OK && FD_ISSET(client->sock_fd, &readfds)) {
        char read_buf[BUF_SIZE] = { 0 };

        ssize_t bytes_read = read(client->sock_fd, read_buf, BUF_SIZE);
        if (bytes_read == 0) {
            client->sock_status = SOCK_DONE;
        }
        if (bytes_read == -1) {
            perror("Unable to read from client socket");
            client->sock_status = SOCK_ERROR;
        }
        if (bytes_read > 0) {
            client->received_response = TRUE;
            read_buf[bytes_read] = '\0';
            printf("Client %d received: %s\n", client->sock_fd, read_buf);
        }
    }
}

void update_read_from_file(list_node_t *client) {
    if (client->buf_size == 0 && client->received_response && client->file_status == FILE_OK && FD_ISSET(client->file_fd, &readfds)) {
        ssize_t bytes_read = read_line(client->file_fd, client->buf, FILE_READ_BUF_SIZE);
        if (bytes_read == -1) {
            perror("Unable to read from file");
            client->file_status = FILE_ERROR;
        }
        else {
            if (bytes_read == 0) {
                client->file_status = FILE_DONE;
            }
            client->buf_size = bytes_read;
        }
    }
}

void update_write_to_client(list_node_t *client) {
    if (client->buf_size > 0 && client->sock_status == SOCK_OK && FD_ISSET(client->sock_fd, &writefds)) {
        ssize_t bytes_written = write(client->sock_fd, client->buf, client->buf_size);
        memset(client->buf, 0, BUF_SIZE);
        client->buf_size = 0;
        client->received_response = 0;
        if (bytes_written == -1) {
            perror("Unable to write to client socket");
            client->sock_status = SOCK_ERROR;
        }
    }
}

void update_clients() {
    list_node_t *cur = clients_list.head;
    while (cur != NULL) {
        list_node_t *next = cur->next;
        update_read_from_client(cur);
        update_read_from_file(cur);
        update_write_to_client(cur);
        cur = next;
    }
}

int check_accept() {
    if (FD_ISSET(listen_fd, &readfds)) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd == -1) {
            perror("accept error");
            return -1;
        }
        list_node_t *node = create_client(client_fd, file_index_offset);
        if (node != NULL) list_add(node);
        file_index_offset = (file_index_offset + 1) % num_of_files;
    }
    return 0;
}

void remote_spin() {
    while(should_work) {
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

int parse_ports(char *listen_port_str, int *listen_port) {
    if (convert_number(listen_port_str, listen_port) == -1) {
        return -1;
    }

    if (!IS_PORT_VALID(*listen_port)) {
        fprintf(stderr, "Invalid port: listen_port=%d\n", *listen_port);
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s files_prefix listen_port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (init_signals() == -1) {
        return EXIT_FAILURE;
    }

    prefix = argv[1];
    int listen_port;
    if (parse_ports(argv[2], &listen_port) == -1) {
        return EXIT_FAILURE;
    }

    calc_max_file_index();
    if (num_of_files <= 0) {
        fprintf(stderr, "No files of form '%s' exists\n", prefix);
        return EXIT_FAILURE;
    }

    if ((listen_fd = open_listen_socket(listen_port)) == -1) {
        return EXIT_FAILURE;
    }

    remote_spin();

    remove_all_clients();
    return EXIT_SUCCESS;
}
