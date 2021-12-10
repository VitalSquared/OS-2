/*
 * This program creates N clients, all of which connect to one server.
 * They periodically send messages to server.
 * Data is being read from files line-by-line.
 * Each client reads his own file.
 * When end of file is reached, client disconnects
 * Use 'filegen.c' to generate files. You have to enter the same prefix you used in 'filegen'. Keep in mind current working directory.
 *
 * Created by Vitaly Spirin, NSU, group 19203
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>

#define SOCK_OK (0)
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)

#define FILE_OK (0)
#define FILE_ERROR (-1)
#define FILE_DONE (-2)

#define DELAY_S 1
#define BUF_SIZE 512
#define FILE_READ_BUF_SIZE 64
#define MAX_NUM_OF_FILES 512
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define IS_PORT_VALID(port) (0 < (port) && (port) <= 0xFFFF)
#define IS_NUM_OF_CLIENTS_VALID(n) (0 < (n) && (n) <= 512)

#define IS_CLIENT_INADEQUATE(client) ((client)->sock_status != SOCK_OK || (client)->file_status != FILE_OK)

#define TRUE 1
#define FALSE 0

const char *prefix = "clients_files/client";
const char file_names[MAX_NUM_OF_FILES][BUF_SIZE];

typedef struct list_node {
    int sock_fd, file_fd;
    int sock_status, file_status;

    char buf[BUF_SIZE];
    ssize_t buf_size;

    time_t last_send_time;

    struct list_node *prev;
    struct list_node *next;
} list_node_t;

typedef struct list {
    list_node_t *head;
} list_t;

int select_max_fd = 0;
int num_of_files = -1;
int should_work = TRUE;
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

const char *get_host_error(int err_code) {
    const char *err_msg;
    switch (err_code) {
        case HOST_NOT_FOUND: err_msg = "Authoritative Answer, Host not found"; break;
        case TRY_AGAIN: err_msg = "Non-Authoritative, Host not found, or SERVFAIL"; break;
        case NO_RECOVERY: err_msg = "Non recoverable errors, FORMERR, REFUSED, NOTIMP"; break;
        case NO_DATA: err_msg = "Valid name, no data record of requested type"; break;
        default: err_msg = "Unknown error"; break;
    }
    return err_msg;
}

int init_remote_addr(char *hostname, int port) {
    int error_code;
    struct hostent *remote_host = getipnodebyname(hostname, AF_INET, 0, &error_code);
    if (remote_host == NULL) {
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(error_code));
        return -1;
    }

    memset(&remote_addr, 0, sizeof(struct sockaddr_in));

    remote_addr.sin_family = AF_INET;
    memcpy(&remote_addr.sin_addr.s_addr, remote_host->h_addr_list[0], sizeof(struct in_addr));
    remote_addr.sin_port = htons(port);

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

list_node_t *create_client(int file_index) {
    list_node_t *client = (list_node_t *)calloc(1, sizeof(list_node_t));
    if (client == NULL) {
        perror("Unable to allocate memory for client");
        return NULL;
    }

    if ((client->sock_fd = open_remote_socket()) == -1) {
        perror("Unable to create socket for client");
        free(client);
        return NULL;
    }

    int file_fd = open(file_names[file_index], O_RDONLY);
    if (file_fd == -1) {
        perror(file_names[file_index]);
        close(client->sock_fd);
        free(client);
        return NULL;
    }

    client->file_fd = file_fd;
    client->sock_status = SOCK_OK;
    client->file_status = FILE_OK;
    client->last_send_time = time(NULL) - DELAY_S;

    printf("Created client %d\n", client->sock_fd);

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
        if (cur->buf_size == 0 && cur->file_status == FILE_OK && (time(NULL) - cur->last_send_time >= DELAY_S)) {
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
            read_buf[bytes_read] = '\0';
            printf("Client %d received: %s\n", client->sock_fd, read_buf);
        }
    }
}

void update_read_from_file(list_node_t *client) {
    if (client->buf_size == 0 && client->file_status == FILE_OK &&  FD_ISSET(client->file_fd, &readfds)) {
        ssize_t bytes_read = read_line(client->file_fd, client->buf, FILE_READ_BUF_SIZE + 1);
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
        client->last_send_time = time(NULL);
        if (bytes_written == -1) {
            perror("Unable to write to client socket");
            client->buf_size = -1;
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

void clients_spin() {
    struct timeval timeout;
    timeout.tv_sec = DELAY_S;
    timeout.tv_usec = 0;

    while(should_work && clients_list.head != NULL) {
        init_select_masks();

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, &timeout);
        if (num_fds_ready == -1) {
            perror("select error");
            break;
        }
        if (num_fds_ready == 0) {
            continue;
        }

        update_clients();
    }
}

void spawn_clients(int num_of_clients) {
    for (int i = 0; i < num_of_clients; i++) {
        list_node_t *client = create_client(i % num_of_files);
        if (client != NULL) list_add(client);
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

int parse_args(char *num_of_clients_str, char *port_str, int *num_of_clients, int *port) {
    if (convert_number(num_of_clients_str, num_of_clients) == -1 || convert_number(port_str, port) == -1) {
        return -1;
    }

    if (!IS_NUM_OF_CLIENTS_VALID(*num_of_clients) || !IS_PORT_VALID(*port)) {
        fprintf(stderr, "Must be: (0 < num_of_clients <= 512) and (0 < port < %d)\nGot: num_of_clients=%d, port=%d\n", 0xFFFF, *num_of_clients, *port);
        return EXIT_FAILURE;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s files_prefix num_of_clients hostname port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    if (init_signals() == -1) {
        return EXIT_FAILURE;
    }

    prefix = argv[1];
    char *hostname = argv[3];
    int num_of_clients, port;
    if (parse_args(argv[2], argv[4], &num_of_clients, &port) == -1) {
        return EXIT_FAILURE;
    }

    calc_max_file_index();
    if (num_of_files <= 0) {
        fprintf(stderr, "No files of form '%s' exists\n", prefix);
        return EXIT_FAILURE;
    }

    if (init_remote_addr(hostname, port) == -1) {
        return EXIT_FAILURE;
    }

    spawn_clients(num_of_clients);
    clients_spin();

    remove_all_clients();
    return EXIT_SUCCESS;
}
