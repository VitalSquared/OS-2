#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <aio.h>
#include <errno.h>
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

    size_t pre_read_buf_size;
    struct aiocb sock_aiocb, stdout_aiocb, stdin_aiocb;
    struct aiocb const *aiocb_buf[3];
} data_t;

void print_error(const char *prefix, int code) {
    if (prefix == NULL) {
        prefix = "error";
    }
    char buf[256];
    if (strerror_r(code, buf, sizeof(buf)) != 0) {
        sprintf(buf, "code %d", code);
    }
    fprintf(stderr, "%s: %s\n", prefix, buf);
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

    memset(&data->sock_aiocb, 0, sizeof(struct aiocb));
    data->sock_aiocb.aio_fildes = data->sock_fd;

    memset(&data->stdout_aiocb, 0, sizeof(struct aiocb));
    data->stdout_aiocb.aio_fildes = STDOUT_FILENO;

    memset(&data->stdin_aiocb, 0, sizeof(struct aiocb));
    data->stdin_aiocb.aio_fildes = STDIN_FILENO;

    for (int i = 0; i < 3; i++) {
        data->aiocb_buf[i] = NULL;
    }

    return 0;
}

void init_aio_read_from_sock(data_t *data) {
    data->pre_read_buf_size = data->buf_size;
    data->sock_aiocb.aio_buf = data->buf + data->buf_size;
    data->sock_aiocb.aio_nbytes = BUF_SIZE - data->buf_size;

    if (aio_read(&data->sock_aiocb) == -1) {
        perror("init_aio_read_from_sock");
        data->sock_status = SOCK_ERROR;
        return;
    }

    data->aiocb_buf[0] = &data->sock_aiocb;
}

void init_aio_write_to_stdout(data_t *data) {
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

    data->stdout_aiocb.aio_buf = data->buf;
    data->stdout_aiocb.aio_nbytes = output_length;

    if (aio_write(&data->stdout_aiocb) == -1) {
        perror("init_aio_read_from_sock");
        data->stdout_status = STREAM_ERROR;
        return;
    }

    data->aiocb_buf[1] = &data->stdout_aiocb;
}

void init_aio_read_from_stdin(data_t *data) {
    data->stdin_aiocb.aio_buf = data->stdin_buf;
    data->stdin_aiocb.aio_nbytes = BUF_SIZE;

    if (aio_read(&data->stdin_aiocb) == -1) {
        perror("init_aio_read_from_sock");
        data->stdin_status = STREAM_ERROR;
        return;
    }

    data->aiocb_buf[2] = &data->stdin_aiocb;
}

int init_aio(data_t *data) {
    if (IS_DATA_INADEQUATE(data)) {
        return -1;
    }

    if (!IS_BUF_FULL(data) && data->sock_status == SOCK_OK && data->sock_aiocb.aio_nbytes == 0) {
        init_aio_read_from_sock(data);
    }
    if (!IS_BUF_EMPTY(data) && data->processed_lines < MAX_NUM_OF_LINES && data->stdout_status == STREAM_OK && data->stdout_aiocb.aio_nbytes == 0) {
        init_aio_write_to_stdout(data);
    }
    if (data->processed_lines >= MAX_NUM_OF_LINES && data->stdin_status == STREAM_OK && data->stdin_aiocb.aio_nbytes == 0) {
        init_aio_read_from_stdin(data);
    }

    return 0;
}

void read_from_socket(data_t *data) {
    if (data->sock_aiocb.aio_nbytes == 0) {
        return;
    }

    int err_code = aio_error(&data->sock_aiocb);
    if (err_code == EINPROGRESS) {
        return;
    }
    if (err_code != 0) {
        if (err_code < 0) perror("read_from_socket");
        else print_error("read_from_socket", err_code);
        data->sock_status = SOCK_ERROR;
        data->sock_aiocb.aio_nbytes = 0;
        data->aiocb_buf[0] = NULL;
        return;
    }
    
    ssize_t bytes_read = aio_return(&data->sock_aiocb);
    if (bytes_read == -1) {
        perror("Unable to read from socket");
        data->sock_status = SOCK_ERROR;
        data->sock_aiocb.aio_nbytes = 0;
        data->aiocb_buf[0] = NULL;
        return;
    }
    if (bytes_read == 0) {
        data->sock_status = SOCK_DONE;
        data->sock_aiocb.aio_nbytes = 0;
        data->aiocb_buf[0] = NULL;
        return;
    }

    if (data->buf_size < data->pre_read_buf_size) {
        memmove(data->buf + data->buf_size, data->buf + data->pre_read_buf_size, bytes_read);
    }
    data->buf_size += bytes_read;
    data->sock_aiocb.aio_nbytes = 0;
    data->aiocb_buf[0] = NULL;
}

void write_to_stdout(data_t *data) {
    if (data->stdout_aiocb.aio_nbytes == 0) {
        return;
    }

    int err_code = aio_error(&data->stdout_aiocb);
    if (err_code == EINPROGRESS) {
        return;
    }
    if (err_code != 0) {
        if (err_code < 0) perror("write_to_stdout");
        else print_error("write_to_stdout", err_code);
        data->stdout_status = STREAM_ERROR;
        data->stdout_aiocb.aio_nbytes = 0;
        data->aiocb_buf[1] = NULL;
        return;
    }

    ssize_t bytes_written = aio_return(&data->stdout_aiocb);
    if (bytes_written == -1) {
        perror("Unable to write to stdout");
        data->stdout_status = STREAM_ERROR;
        data->stdout_aiocb.aio_nbytes = 0;
        data->aiocb_buf[1] = NULL;
        return;
    }

    memmove(data->buf, data->buf + bytes_written, data->buf_size - bytes_written);
    data->buf_size -= bytes_written;
    data->stdout_aiocb.aio_nbytes = 0;
    data->aiocb_buf[1] = NULL;
}

void read_from_stdin(data_t *data) {
    if (data->stdin_aiocb.aio_nbytes == 0) {
        return;
    }

    int err_code = aio_error(&data->stdin_aiocb);
    if (err_code == EINPROGRESS) {
        return;
    }
    if (err_code != 0) {
        if (err_code < 0) perror("read_from_stdin");
        else print_error("read_from_stdin", err_code);
        data->stdin_status = STREAM_ERROR;
        data->stdin_aiocb.aio_nbytes = 0;
        data->aiocb_buf[2] = NULL;
        return;
    }

    ssize_t bytes_read = aio_return(&data->stdin_aiocb);
    if (bytes_read == -1) {
        perror("Unable to read from socket");
        data->stdin_status = STREAM_ERROR;
        data->stdin_aiocb.aio_nbytes = 0;
        data->aiocb_buf[2] = NULL;
        return;
    }

    data->processed_lines = 0;
    data->stdin_aiocb.aio_nbytes = 0;
    data->aiocb_buf[2] = NULL;
}

void http_spin(url_t *url) {
    data_t data;
    if (init_data(url, &data) == -1) {
        return;
    }

    while (!(data.sock_status == SOCK_DONE && IS_BUF_EMPTY(&data))) {
        if (init_aio(&data) == -1) {
            break;
        }

        if (aio_suspend(data.aiocb_buf, 3, NULL) == -1) {
            perror("aio_suspend error");
            break;
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
