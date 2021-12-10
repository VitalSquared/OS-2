/*
 * This program creates a listener.
 * When client connects, it starts sending responses for every message from client.
 * Data is being read from files line-by-line.
 * When end of file is reached, we wait for client to disconnect
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>

#define BUF_SIZE 512
#define FILE_READ_BUF_SIZE 64
#define MAX_NUM_OF_FILES 512
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define IS_PORT_VALID(port) (0 < (port) && (port) <= 0xFFFF)
#define IS_NUM_OF_CLIENTS_VALID(n) (0 < (n) && (n) <= 512)

const char *prefix = "remote_files/remote_";
const char file_names[MAX_NUM_OF_FILES][BUF_SIZE];

typedef struct client {
    int sock_fd, file_fd;

    char buf[BUF_SIZE];
    ssize_t buf_size;

    int received_response;

    struct client *prev;
    struct client *next;
} client_t;

int listen_fd;
int select_max_fd = 0;
int num_of_files = -1;
int file_index_offset = 0;
fd_set readfds, writefds;
client_t *head = NULL, *tail = NULL;

void calc_max_file_index() {
    struct stat statbuf;
    int i;

    for (i = 0; i < MAX_NUM_OF_FILES; i++) {
        char name[BUF_SIZE] = { 0 };
        sprintf(name, "%s%d.txt", prefix, i);
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
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1) {
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
        case TRY_AGAIN: err_msg = "Non-Authoritative, Host not found, or SERVFAIL"; break;
        case NO_RECOVERY: err_msg = "Non recoverable errors, FORMERR, REFUSED, NOTIMP"; break;
        case NO_DATA: err_msg = "Valid name, no data record of requested type"; break;
        default: err_msg = "Unknown error"; break;
    }
    return err_msg;
}

int init_remote_addr(char *hostname, int port, struct sockaddr_in *addr) {
    int error_code;
    struct hostent *remote_host = getipnodebyname(hostname, AF_INET, 0, &error_code);
    if (remote_host == NULL) {
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(error_code));
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    memcpy(&addr->sin_addr.s_addr, remote_host->h_addr_list[0], sizeof(struct in_addr));
    addr->sin_port = htons(port);
    freehostent(remote_host);

    return 0;
}

void create_client(int sock_fd, int file_index) {
    client_t *client = (client_t *)malloc(sizeof(client_t));
    if (client == NULL) {
        perror("Unable to allocate memory for client");
        return;
    }

    client->sock_fd = sock_fd;

    int file_fd = open(file_names[file_index], O_RDONLY);
    if (file_fd == -1) {
        perror(file_names[file_index]);
        close(client->sock_fd);
        free(client);
        return;
    }

    client->prev = NULL;
    client->next = head;
    if (head == NULL) tail = client;
    else head->prev = client;
    head = client;

    client->received_response = 0;
    client->file_fd = file_fd;
    client->buf_size = 0;
    memset(client->buf, 0, BUF_SIZE);

    printf("New client %d connected\n", client->sock_fd);
    select_max_fd = MAX(select_max_fd, client->sock_fd);
    select_max_fd = MAX(select_max_fd, client->file_fd);
}

void drop_client(client_t *client) {
    if (client == head && client == tail) {
        head = NULL;
        tail = NULL;
    }
    else if (client == head) {
        head = client->next;
        head->prev = NULL;
    }
    else if (client == tail) {
        tail = client->prev;
        tail->next = NULL;
    }
    else {
        client->prev->next = client->next;
        client->next->prev = client->prev;
    }

    printf("Client %d disconnected\n", client->sock_fd);

    close(client->sock_fd);
    close(client->file_fd);
    free(client);
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

    client_t *cur = head;
    while (cur != NULL) {
        client_t *next = cur->next;

        if (cur->buf_size < 0) {
            drop_client(cur);
        }
        else {
            FD_SET(cur->sock_fd, &readfds);
            if (cur->buf_size == 0 && cur->received_response) {
                FD_SET(cur->file_fd, &readfds);
            }
            if (cur->buf_size > 0) {
                FD_SET(cur->sock_fd, &writefds);
            }
        }

        cur = next;
    }
}

void update_clients() {
    client_t *cur = head;
    char read_buf[BUF_SIZE] = { 0 };
    while (cur != NULL) {
        client_t *next = cur->next;

        if (FD_ISSET(cur->sock_fd, &readfds)) {
            ssize_t bytes_read = read(cur->sock_fd, read_buf, BUF_SIZE);
            if (bytes_read == 0) {
                cur->buf_size = -1;
            }
            if (bytes_read == -1) {
                perror("Unable to read from client socket");
                cur->buf_size = -1;
            }
            if (bytes_read > 0) {
                cur->received_response = 1;
                read_buf[bytes_read] = '\0';
                printf("Client %d received: %s\n", cur->sock_fd, read_buf);
            }
        }

        if (cur->buf_size == 0 && cur->received_response && FD_ISSET(cur->file_fd, &readfds)) {
            ssize_t bytes_read = read_line(cur->file_fd, cur->buf, FILE_READ_BUF_SIZE);
            if (bytes_read == -1) {
                perror("Unable to read from file");
            }
            cur->buf_size = bytes_read;
        }

        if (cur->buf_size > 0 && FD_ISSET(cur->sock_fd, &writefds)) {
            ssize_t bytes_written = write(cur->sock_fd, cur->buf, cur->buf_size);
            memset(cur->buf, 0, BUF_SIZE);
            cur->buf_size = 0;
            cur->received_response = 0;
            if (bytes_written == -1) {
                perror("Unable to write to client socket");
                cur->buf_size = -1;
            }
        }

        cur = next;
    }
}

void disconnect_all_clients() {
    client_t *cur = head;
    while (cur != NULL) {
        client_t *next = cur->next;
        drop_client(cur);
        cur = next;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s listen_port\n", argv[0]);
        return EXIT_SUCCESS;
    }

    int listen_port = atoi(argv[1]);
    if (!IS_PORT_VALID(listen_port)) {
        fprintf(stderr, "Must be: (0 < listen_port < %d)\nGot: listen_port=%d\n", 0xFFFF, listen_port);
        return EXIT_FAILURE;
    }

    calc_max_file_index();
    if (num_of_files <= 0) {
        fprintf(stderr, "No files of form 'remote_x.txt' exists\n");
        return EXIT_FAILURE;
    }

    if ((listen_fd = open_listen_socket(listen_port)) == -1) {
        return EXIT_FAILURE;
    }

    while(1) {
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
        if (FD_ISSET(listen_fd, &readfds)) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd == -1) {
                perror("accept error");
                break;
            }
            create_client(client_fd, file_index_offset);
            file_index_offset = (file_index_offset + 1) % num_of_files;
        }
    }

    disconnect_all_clients();
    return EXIT_SUCCESS;
}
