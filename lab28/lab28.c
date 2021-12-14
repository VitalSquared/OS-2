#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include "url_parser.h"

#define BUF_SIZE 4096
#define MAX_NUM_OF_LINES 25
#define MAX_BYTES_PER_LINE 200

#define IS_BUF_EMPTY(data) ((data)->buf_size == 0)
#define IS_BUF_FULL(data) ((data)->buf_size == BUF_SIZE)
#define IS_DATA_INADEQUATE(data) ((data)->sock_status == SOCK_ERROR || (data)->stdin_status == STREAM_ERROR || (data)->stdout_status == STREAM_ERROR)

#define SOCK_OK (0)
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)

#define STREAM_OK (0)
#define STREAM_ERROR (-1)

#define TRUE 1
#define FALSE 0

typedef struct data {
    int sock_fd;
    int sock_status, stdin_status, stdout_status;

    int processed_lines;
    int current_line_bytes;

    size_t buf_size;
    char buf[BUF_SIZE];
    char stdin_buf[BUF_SIZE];

    fd_set readfds, writefds;
} data_t;

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

int open_socket(char *hostname, int port) {
    int err_code;
    struct hostent *server_host = getipnodebyname(hostname, AF_INET, 0, &err_code);
    if (server_host == NULL) {
        fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(err_code));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server_host->h_addr_list[0], sizeof(struct in_addr));

    freehostent(server_host);

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket error");
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
        perror("connect error");
        return -1;
    }

    return sock_fd;
}

int send_get_request(int sock_fd, url_t *url) {
    char buf[BUF_SIZE] = { 0 };
    sprintf(buf, "GET %s HTTP/1.0\r\n\r\n", url->full);

    ssize_t bytes_written = write(sock_fd, buf, strlen(buf));
    if (bytes_written == -1) {
        perror("Unable to write GET request to socket");
        return -1;
    }

    return 0;
}

int init_data(url_t *url, data_t *data) {
    int sock_fd;
    if ((sock_fd = open_socket(url->hostname, url->port)) == -1) {
        return -1;
    }
    if (send_get_request(sock_fd, url) == -1) {
        close(sock_fd);
        return -1;
    }

    memset(data, 0, sizeof(data_t));
    data->sock_fd = sock_fd;
    data->sock_status = SOCK_OK;
    data->stdin_status = STREAM_OK;
    data->stdout_status = STREAM_OK;

    return 0;
}

int init_select_masks(data_t *data) {
    FD_ZERO(&data->readfds);
    FD_ZERO(&data->writefds);

    if (IS_DATA_INADEQUATE(data)) {
        return -1;
    }

    if (!IS_BUF_FULL(data) && data->sock_status == SOCK_OK) {
        FD_SET(data->sock_fd, &data->readfds);
    }
    if (!IS_BUF_EMPTY(data) && data->processed_lines < MAX_NUM_OF_LINES && data->stdout_status == STREAM_OK) {
        FD_SET(STDOUT_FILENO, &data->writefds);
    }
    if (data->processed_lines >= MAX_NUM_OF_LINES && data->stdin_status == STREAM_OK) {
        FD_SET(STDIN_FILENO, &data->readfds);
    }

    return 0;
}

void read_from_socket(data_t *data) {
    if (!IS_BUF_FULL(data) && data->sock_status == SOCK_OK && FD_ISSET(data->sock_fd, &data->readfds)) {
        ssize_t bytes_read = read(data->sock_fd, data->buf + data->buf_size, BUF_SIZE - data->buf_size);

        if (bytes_read == -1) {
            perror("Unable to read from socket");
            data->sock_status = SOCK_ERROR;
            return;
        }
        if (bytes_read == 0) {
            data->sock_status = SOCK_DONE;
            return;
        }

        data->buf_size += bytes_read;
    }
}

void write_to_stdout(data_t *data) {
    if (!IS_BUF_EMPTY(data) && data->processed_lines < MAX_NUM_OF_LINES && data->stdout_status == STREAM_OK && FD_ISSET(STDOUT_FILENO, &data->writefds)) {
        size_t output_length = 0;
        for (size_t i = 0; i < data->buf_size; i++) {
            output_length++;
            data->current_line_bytes++;
            if (data->buf[i] == '\n' || data->current_line_bytes >= MAX_BYTES_PER_LINE - 1) {
                data->current_line_bytes = 0;
                data->processed_lines++;
                if (data->processed_lines >= MAX_NUM_OF_LINES) {
                    break;
                }
            }
        }

        ssize_t bytes_written = write(STDOUT_FILENO, data->buf, output_length);
        if (bytes_written == -1) {
            perror("Unable to write to STDOUT");
            data->stdout_status = STREAM_ERROR;
            return;
        }

        memmove(data->buf, data->buf + bytes_written, data->buf_size - bytes_written);
        data->buf_size -= bytes_written;
    }
}

void read_from_stdin(data_t *data) {
    if (data->processed_lines >= MAX_NUM_OF_LINES && data->stdin_status == STREAM_OK && FD_ISSET(STDIN_FILENO, &data->readfds)) {
        if (read(STDIN_FILENO, data->stdin_buf, BUF_SIZE) == -1) {
            perror("Unable to read from STDIN");
            data->stdin_status = STREAM_ERROR;
            return;
        }
        data->processed_lines = 0;
    }
}

void http_spin(url_t *url) {
    data_t data;
    if (init_data(url, &data) == -1) {
        return;
    }

    while (!(data.sock_status == SOCK_DONE && IS_BUF_EMPTY(&data))) {
        if (init_select_masks(&data) == -1) {
            break;
        }

        int num_fds = select(data.sock_fd + 1, &data.readfds, &data.writefds, NULL, NULL);
        if (num_fds == -1) {
            perror("select error");
            break;
        }
        if (num_fds == 0) {
            continue;
        }

        read_from_socket(&data);
        write_to_stdout(&data);
        read_from_stdin(&data);
    }

    close(data.sock_fd);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s url\n", argv[0]);
        return EXIT_SUCCESS;
    }

    url_t *url = parse_url(argv[1], 80);
    if (url == NULL) {
        fprintf(stderr, "Unable to parse URL\n");
        return EXIT_FAILURE;
    }
    if (strcmp(url->protocol, "http") != 0) {
        fprintf(stderr, "Only HTTP protocol is supported\n");
        free_url(url);
        return EXIT_FAILURE;
    }
    if (url->user != NULL) {
        fprintf(stderr, "HTTP authentication is not supported\n");
        free_url(url);
        return EXIT_FAILURE;
    }
    if (url->port == URL_PORT_ERROR) {
        fprintf(stderr, "Port parsing error\n");
        free_url(url);
        return EXIT_FAILURE;
    }

    http_spin(url);
    printf("\n");
    free_url(url);
    return EXIT_SUCCESS;
}
