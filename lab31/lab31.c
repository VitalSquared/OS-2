#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include "cache.h"
#include "picohttpparser.h"

//#define DROP_HTTP_NO_CLIENTS

#define BUF_SIZE 4096

#define INTERNAL_ERROR 0
#define CONNECTION_ERROR 1
#define INVALID_REQUEST 2
#define NOT_A_GET_METHOD 3
#define HOST_NOT_FOUND_CUSTOM 4
#define TRY_AGAIN_CUSTOM 5
#define NO_RECOVERY_CUSTOM 6
#define NO_DATA_CUSTOM 7
#define UNKNOWN_ERROR 8

#define GETTING_FROM_CACHE 2    //only for client
#define DOWNLOADING 1
#define AWAITING_REQUEST 0
#define SOCK_ERROR (-1)
#define SOCK_DONE (-2)
#define NON_SOCK_ERROR (-3)

#define HTTP_NO_HEADERS (-1)

#define HTTP_CODE_UNDEFINED (-1)
#define HTTP_CODE_NONE 0

#define HTTP_CONTENT_LENGTH (1)
#define HTTP_CHUNKED (0)

#define TRUE 1
#define FALSE 0

#define INFO_LOG TRUE
#define ERROR_LOG TRUE

#define STR_EQ(STR1, STR2) (strcmp(STR1, STR2) == 0)
#define IS_PORT_VALID(PORT) (0 < (PORT) && (PORT) <= 0xFFFF)
#define MAX(A, B) ((A) > (B) ? (A) : (B))

typedef struct http {
    int sock_fd, code, clients, status, error, is_response_complete;
    int response_type, headers_size; ssize_t response_size;
    struct phr_chunked_decoder decoder;
    char *data;     ssize_t data_size;
    char *request;  ssize_t request_size;   ssize_t request_bytes_written;
    char *host, *path;
    cache_entry_t *cache_entry;
    struct http *prev, *next;
} http_t;

typedef struct client {
    int sock_fd, status, error;
    cache_entry_t *cache_entry;  http_t *http_entry;
    char *request;  ssize_t request_size;
    ssize_t bytes_written;
    struct client *prev, *next;
} client_t;

typedef struct client_list {
    client_t *head;
} client_list_t;

typedef struct http_list {
    http_t *head;
} http_list_t;

int listen_fd;
cache_t cache;

fd_set readfds, writefds;
int select_max_fd = STDIN_FILENO;

client_list_t client_list = { .head = NULL };
http_list_t http_list = { .head = NULL };

int convert_number(char *str, int *number) {
    errno = 0;
    char *endptr = "";
    long num = strtol(str, &endptr, 10);

    if (errno != 0) {
        if (ERROR_LOG) perror("Can't convert given number");
        return -1;
    }
    if (strcmp(endptr, "") != 0) {
        if (ERROR_LOG) fprintf(stderr, "Number contains invalid symbols\n");
        return -1;
    }

    *number = (int)num;
    return 0;
}

int strings_equal_by_length(const char *str1, size_t len1, const char *str2, size_t len2) {
    if (len1 != len2) return FALSE;
    if (str1 == NULL || str2 == NULL) return FALSE;
    for (size_t i = 0; i < len1; i++) {
        if (str1[i] != str2[i]) return FALSE;
    }
    return TRUE;
}

int get_number_from_string_by_length(const char *str, size_t length) {
    char buf1[length + 1];
    memcpy(buf1, str, length);
    buf1[length] = '\0';
    int num = -1;
    convert_number(buf1, &num);
    return num;
}

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

const char *get_host_error(int err_code, int *out_error) {
    const char *err_msg;
    switch (err_code) {
        case HOST_NOT_FOUND: err_msg = "Authoritative Answer, Host not found"; *out_error = HOST_NOT_FOUND_CUSTOM; break;
        case TRY_AGAIN: err_msg = "Non-Authoritative, Host not found, or SERVERFAIL"; *out_error = TRY_AGAIN_CUSTOM; break;
        case NO_RECOVERY: err_msg = "Non recoverable errors, FORMERR, REFUSED, NOTIMP"; *out_error = NO_RECOVERY_CUSTOM; break;
        case NO_DATA: err_msg = "Valid name, no data record of requested type"; *out_error = NO_DATA_CUSTOM; break;
        default: err_msg = "Unknown error"; break;
    }
    return err_msg;
}

int open_http_socket(const char *hostname, int port, int *out_error) {
    int err_code;
    struct hostent *server_host = getipnodebyname(hostname, AF_INET, 0, &err_code);
    if (server_host == NULL) {
        if (ERROR_LOG) fprintf(stderr, "Unable to connect to host %s: %s\n", hostname, get_host_error(err_code, out_error));
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
        if (ERROR_LOG) perror("open_http_socket: socket error");
        *out_error = INTERNAL_ERROR;
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
        if (ERROR_LOG) perror("open_http_socket: connect error");
        *out_error = CONNECTION_ERROR;
        close(sock_fd);
        return -1;
    }

    if (fcntl(sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("open_http_socket: fcntl error");
    }

    return sock_fd;
}

http_t *create_http(int sock_fd, char *request, ssize_t request_size, char *host, char *path) {
    http_t *new_http = (http_t *)calloc(1, sizeof(http_t));
    if (new_http == NULL) {
        if (ERROR_LOG) perror("create_http: Unable to allocate memory for http struct");
        return NULL;
    }

    //add http to list
    new_http->prev = NULL;
    new_http->next = http_list.head;
    http_list.head = new_http;
    if (new_http->next != NULL) new_http->next->prev = new_http;

    //init http data
    new_http->status = AWAITING_REQUEST;
    new_http->clients = 1;  //we create http if there is a request, so we already have 1 client
    new_http->data = NULL; new_http->data_size = 0;
    new_http->code = HTTP_CODE_UNDEFINED;
    new_http->headers_size = HTTP_NO_HEADERS;
    new_http->is_response_complete = FALSE;
    new_http->decoder.consume_trailer = 1;
    new_http->sock_fd = sock_fd;
    new_http->request = request; new_http->request_size = request_size; new_http->request_bytes_written = 0;
    new_http->host = host; new_http->path = path;
    new_http->cache_entry = NULL;

    if (INFO_LOG) printf("[%s %s] Connected\n", host, path);
    return new_http;
}

void remove_http(http_t *http) {
    if (http == http_list.head) {
        http_list.head = http->next;
        if (http_list.head != NULL) http_list.head->prev = NULL;
    }
    else {
        http->prev->next = http->next;
        if (http->next != NULL) http->next->prev = http->prev;
    }

    if (INFO_LOG) printf("[%d %s %s] Disconnected\n", http->sock_fd, http->host, http->path);

    if (http->cache_entry == NULL) {
        free(http->data);
        free(http->host);
        free(http->path);
    }
    free(http);
}

void create_client(int client_sock_fd) {
    if (fcntl(client_sock_fd, F_SETFL, O_NONBLOCK) == -1) {
        if (ERROR_LOG) perror("create_client: fcntl error");
    }

    client_t *new_client = (client_t *)calloc(1, sizeof(client_t));
    if (new_client == NULL) {
        if (ERROR_LOG) perror("create_client: Unable to allocate memory for client struct");
        close(client_sock_fd);
        return;
    }
    select_max_fd = MAX(select_max_fd, client_sock_fd);

    //add client to list
    new_client->prev = NULL;
    new_client->next = client_list.head;
    client_list.head = new_client;
    if (new_client->next != NULL) new_client->next->prev = new_client;

    //init client data
    new_client->sock_fd = client_sock_fd;
    new_client->status = AWAITING_REQUEST;
    new_client->cache_entry = NULL;
    new_client->http_entry = NULL;
    new_client->bytes_written = 0;
    new_client->request = NULL;
    new_client->request_size = 0;

    if (INFO_LOG) printf("[%d] Connected\n", client_sock_fd);
}

void remove_client(client_t *client) {
    if (client == client_list.head) {
        client_list.head = client->next;
        if (client_list.head != NULL) client_list.head->prev = NULL;
    }
    else {
        client->prev->next = client->next;
        if (client->next != NULL) client->next->prev = client->prev;
    }

    if (INFO_LOG) printf("[%d] Disconnected\n", client->sock_fd);

    if (client->http_entry != NULL) client->http_entry->clients--;
    close(client->sock_fd);
    free(client);
}

void remove_all_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        remove_client(cur_client);
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        if (cur_http->status == DOWNLOADING) close(cur_http->sock_fd);
        remove_http(cur_http);
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

int parse_client_request(client_t *client, char **host, char **path, ssize_t bytes_read) {
    const char *method, *phr_path;
    size_t method_len, path_len;
    int minor_version;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int err_code = phr_parse_request(client->request, client->request_size, &method, &method_len, &phr_path, &path_len, &minor_version, headers, &num_headers, client->request_size - bytes_read);
    if (err_code == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to parse request\n");
        client->status = NON_SOCK_ERROR; client->error = INVALID_REQUEST;
        client->bytes_written = 0;
        return -1;
    }
    if (err_code == -2) return -2; //incomplete, read from client more

    if (!strings_equal_by_length(method, method_len, "GET", 3)) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: not a GET method\n");
        client->status = NON_SOCK_ERROR; client->error = NOT_A_GET_METHOD;
        client->bytes_written = 0;
        return -1;
    }

    *path = (char *)calloc(path_len + 1, sizeof(char));
    if (*path == NULL) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for path\n");
        client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
        client->bytes_written = 0;
        return -1;
    }
    memcpy(*path, phr_path, path_len);

    int found_host = FALSE;
    for (size_t i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len,  "Host", 4)) {
            *host = calloc(headers[i].value_len + 1, sizeof(char));
            if (*host == NULL) {
                if (ERROR_LOG) fprintf(stderr, "parse_client_request: unable to allocate memory for host\n");
                free(*path); *path = NULL;
                client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
                client->bytes_written = 0;
                return -1;
            }
            memcpy(*host, headers[i].value, headers[i].value_len);
            found_host = TRUE;
            break;
        }
    }
    if (!found_host) {
        if (ERROR_LOG) fprintf(stderr, "parse_client_request: no host header\n");
        free(*path); *path = NULL;
        client->status = NON_SOCK_ERROR; client->error = INVALID_REQUEST;
        client->bytes_written = 0;
        return -1;
    }

    return 0;
}

void handle_client_request(client_t *client, ssize_t bytes_read) {
    char *host = NULL, *path = NULL;
    int err_code = parse_client_request(client, &host, &path, bytes_read);
    if (err_code == -1) {
        client->request_size = 0;
        free(client->request); client->request = NULL;
        return;
    }
    if (err_code == -2) return;

    cache_entry_t *entry = cache_find(host, path, &cache);
    if (entry != NULL && entry->is_full) {
        if (INFO_LOG) printf("[%d] Getting data from cache for '%s%s'\n", client->sock_fd, host, path);
        client->status = GETTING_FROM_CACHE;
        client->cache_entry = entry;
        client->request_size = 0;
        free(client->request); client->request = NULL;
        free(host); free(path);
        return;
    }

    //there is no entry in cache:
    http_t *http_entry = http_list.head;
    while (http_entry != NULL) {    //we look for already existing http connection with the same request
        if (STR_EQ(http_entry->host, host) && STR_EQ(http_entry->path, path) &&
            (http_entry->status == DOWNLOADING || http_entry->status == SOCK_DONE)) {
            break;
        }
        http_entry = http_entry->next;
    }

    if (http_entry != NULL) {   //there is active http entry with the same request
        client->request_size = 0;
        free(client->request); client->request = NULL;
        http_entry->clients++;
    }
    else {  //no active http entry with the same request
        int http_sock_fd = open_http_socket(host, 80, &err_code);
        if (http_sock_fd == -1) {
            client->status = NON_SOCK_ERROR; client->error = err_code;
            client->bytes_written = 0;
            client->request_size = 0;
            free(client->request); client->request = NULL;
            free(host); free(path);
            return;
        }

        http_entry = create_http(http_sock_fd, client->request, client->request_size, host, path);
        if (http_entry == NULL) {
            client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
            client->bytes_written = 0;
            client->request_size = 0;
            free(client->request); client->request = NULL;
            free(host); free(path);
            close(http_sock_fd);
            return;
        }

        select_max_fd = MAX(select_max_fd, http_sock_fd);
        client->request_size = 0;
        client->request = NULL;
    }

    client->status = DOWNLOADING;
    client->http_entry = http_entry;
    if (INFO_LOG) printf("[%d] No data in cache for '%s %s'.\n", client->sock_fd, host, path);
}

void read_data_from_client(client_t *client) {
    char buf[BUF_SIZE + 1];
    errno = 0;
    ssize_t bytes_read = recv(client->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("read_data_from_client: Unable to read from client socket");
        client->status = SOCK_ERROR;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }
    if (bytes_read == 0) {
        client->status = SOCK_DONE;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }

    if (client->status != AWAITING_REQUEST) {
        if ((client->status == DOWNLOADING && client->bytes_written == client->http_entry->data_size) ||
            (client->status == GETTING_FROM_CACHE && client->bytes_written == client->cache_entry->size)) {
            if (client->http_entry != NULL) {
                client->http_entry->clients--;
                client->http_entry = NULL;
            }
            client->bytes_written = 0;
            client->status = AWAITING_REQUEST;
            client->request_size = 0;
            free(client->request);  client->request = NULL;
        }
        else {
            if (ERROR_LOG) fprintf(stderr, "read_data_from_client: client read data when we shouldn't\n");
            if (INFO_LOG) {
                buf[bytes_read] = '\n';
                write(STDERR_FILENO, buf, bytes_read + 1);
            }
            return;
        }
    }

    char *check = (char *)realloc(client->request, client->request_size + BUF_SIZE);
    if (check == NULL) {
        if (ERROR_LOG) perror("read_data_from_client: Unable to reallocate memory for client request");
        client->status = NON_SOCK_ERROR; client->error = INTERNAL_ERROR;
        client->bytes_written = 0;
        client->request_size = 0;
        free(client->request);  client->request = NULL;
        return;
    }

    client->request = check;
    memcpy(client->request + client->request_size, buf, bytes_read);
    client->request_size += bytes_read;

    handle_client_request(client, bytes_read);
}

void check_finished_writing_to_client(client_t *client) {
    size_t size = 0;

    if (client->status == GETTING_FROM_CACHE) size = client->cache_entry->size;
    else if (client->status == DOWNLOADING) size = client->http_entry->data_size;

    if (client->bytes_written >= size && (
                (client->status == GETTING_FROM_CACHE && client->cache_entry->is_full) ||
                (client->status == DOWNLOADING && client->http_entry->is_response_complete))) {
        client->bytes_written = 0;

        client->cache_entry = NULL;
        if (client->http_entry != NULL) {
            client->http_entry->clients--;
            client->http_entry = NULL;
        }

        client->status = AWAITING_REQUEST;
    }
}

void write_to_client(client_t *client) {
    ssize_t offset = client->bytes_written;
    const char *buf = "";
    ssize_t size = 0;

    if (client->status == GETTING_FROM_CACHE) {
        buf = client->cache_entry->data;
        size = client->cache_entry->size;
    }
    else if (client->status == DOWNLOADING) {
        buf = client->http_entry->data;
        size = client->http_entry->data_size;
    }

    ssize_t bytes_written = write(client->sock_fd, buf + offset, size - offset);
    if (bytes_written == -1) {
        if (ERROR_LOG) perror("write_to_client: Unable to write to client socket");
        client->status = SOCK_ERROR;
        return;
    }
    client->bytes_written += bytes_written;

    check_finished_writing_to_client(client);
}

void parse_http_response_headers(http_t *entry) {
    int minor_version, status;
    const char *msg;
    size_t msg_len;
    struct phr_header headers[100];
    size_t num_headers = sizeof(headers) / sizeof(headers[0]);

    int headers_size = phr_parse_response(entry->data, entry->data_size, &minor_version, &status, &msg, &msg_len, headers, &num_headers, 0);
    if (headers_size == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_http_response: Unable to parse http response headers\n");
        entry->status = NON_SOCK_ERROR;
        entry->error = CONNECTION_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        free(entry->data); entry->data = NULL;
        entry->is_response_complete = FALSE;
        return;
    }

    if (status != 0) entry->code = status; //request may be incomplete, but it managed to get status

    if (headers_size >= 0) entry->headers_size = headers_size;

    for (int i = 0; i < num_headers; i++) {
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Transfer-Encoding", strlen("Transfer-Encoding")) &&
                strings_equal_by_length(headers[i].value, headers[i].value_len, "chunked", strlen("chunked"))) {
            entry->response_type = HTTP_CHUNKED;
        }
        if (strings_equal_by_length(headers[i].name, headers[i].name_len, "Content-Length", strlen("Content-Length"))) {
            entry->response_type = HTTP_CONTENT_LENGTH;
            entry->response_size = get_number_from_string_by_length(headers[i].value, headers[i].value_len);
            if (entry->response_size == -1) {
                entry->status = NON_SOCK_ERROR;
                entry->error = INTERNAL_ERROR;
                entry->is_response_complete = FALSE;
                entry->data_size = 0;
                free(entry->data); entry->data = NULL;
                return;
            }
        }
    }
}

void parse_http_response_chunked(http_t *entry, char *buf, ssize_t offset, ssize_t size) {
    size_t rsize = size;
    ssize_t pret;

    pret = phr_decode_chunked(&entry->decoder, buf + offset, &rsize);
    if (pret == -1) {
        if (ERROR_LOG) fprintf(stderr, "parse_http_response_chunked: Unable to parse response\n");
        entry->status = NON_SOCK_ERROR;
        entry->error = INTERNAL_ERROR;
        entry->is_response_complete = FALSE;
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, &cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;
    }

    if (entry->code == 200) {
        if (entry->cache_entry == NULL) {
            entry->cache_entry = cache_add(entry->host, entry->path, entry->data, entry->data_size, &cache);
            if (entry->cache_entry == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
        }
    }

    if (pret == 0) {
        if (entry->cache_entry != NULL) entry->cache_entry->is_full = TRUE;
        entry->is_response_complete = TRUE;
    }
}

void parse_http_response_by_length(http_t *entry) {
    if (entry->code == 200) {
        if (entry->cache_entry == NULL) {
            entry->cache_entry = cache_add(entry->host, entry->path, entry->data, entry->data_size, &cache);
            if (entry->cache_entry == NULL) entry->code = HTTP_CODE_NONE;
        }
        else {
            entry->cache_entry->data = entry->data;
            entry->cache_entry->size = entry->data_size;
        }
    }
    if (entry->data_size == entry->headers_size + entry->response_size) {
        if (entry->cache_entry != NULL) entry->cache_entry->is_full = TRUE;
        entry->is_response_complete = TRUE;
    }
}

void read_http_data(http_t *entry) {
    char buf[BUF_SIZE];
    errno = 0;
    ssize_t bytes_read = recv(entry->sock_fd, buf, BUF_SIZE, MSG_DONTWAIT);
    if (bytes_read == -1) {
        if (errno == EWOULDBLOCK) return;
        if (ERROR_LOG) perror("read_http_data: Unable to read from http socket");
        entry->status = SOCK_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, &cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;
        return;
    }
    if (bytes_read == 0) {
        entry->status = SOCK_DONE;
        close(entry->sock_fd);
        if (entry->cache_entry != NULL && !entry->cache_entry->is_full) {
            cache_remove(entry->cache_entry, &cache);
        }
        return;
    }

    if (entry->status != DOWNLOADING) {
        if (ERROR_LOG) fprintf(stderr, "read_http_data: reading from http when we shouldn't\n");
        if (INFO_LOG) write(STDERR_FILENO, buf, bytes_read);
        return;
    }

    char *check = (char *)realloc(entry->data, entry->data_size + BUF_SIZE);
    if (check == NULL) {
        if (ERROR_LOG) perror("read_http_data: Unable to reallocate memory for http data");
        entry->status = NON_SOCK_ERROR; entry->error = INTERNAL_ERROR;
        close(entry->sock_fd);
        entry->data_size = 0;
        if (entry->cache_entry != NULL) {
            cache_remove(entry->cache_entry, &cache);
            entry->cache_entry = NULL;
        }
        else free(entry->data);
        entry->data = NULL;
        return;
    }

    entry->data = check;
    memcpy(entry->data + entry->data_size, buf, bytes_read);
    entry->data_size += bytes_read;

    int b_no_headers = entry->headers_size == HTTP_NO_HEADERS;
    if (entry->headers_size == HTTP_NO_HEADERS) parse_http_response_headers(entry);
    if (entry->status == NON_SOCK_ERROR) return;

    if (entry->headers_size >= 0) {
        if (entry->response_type == HTTP_CHUNKED) {
            parse_http_response_chunked(entry, buf, b_no_headers ? entry->headers_size : 0, b_no_headers ? entry->data_size - entry->headers_size : bytes_read);
        }
        else if (entry->response_type == HTTP_CONTENT_LENGTH) {
            parse_http_response_by_length(entry);
        }
    }
}

void send_http_request(http_t *entry) {
    ssize_t bytes_written = write(entry->sock_fd, entry->request + entry->request_bytes_written, entry->request_size - entry->request_bytes_written);

    if (bytes_written >= 0) entry->request_bytes_written += bytes_written;

    if (entry->request_bytes_written == entry->request_size) {
        entry->status = DOWNLOADING;
        entry->request_size = 0;
        free(entry->request); entry->request = NULL;
    }

    if (bytes_written == -1) {
        if (ERROR_LOG) perror("send_http_request: unable to write to http socket");
        entry->status = SOCK_ERROR;
        close(entry->sock_fd);
        entry->request_size = 0;
        free(entry->request); entry->request = NULL;
    }
}

void init_select_masks() {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(listen_fd, &readfds);
    FD_SET(STDIN_FILENO, &readfds);

    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;

        if (cur_client->status == SOCK_ERROR || cur_client->status == SOCK_DONE || cur_client->status == NON_SOCK_ERROR) {
            remove_client(cur_client);
            cur_client = next;
            continue;
        }

        if (cur_client->http_entry != NULL) {
            if (cur_client->http_entry->status == NON_SOCK_ERROR || cur_client->http_entry->status == SOCK_ERROR) {
                cur_client->status = NON_SOCK_ERROR;
                cur_client->error = cur_client->http_entry->status == SOCK_ERROR ? CONNECTION_ERROR : cur_client->http_entry->error;
                cur_client->http_entry->clients--;
                cur_client->http_entry = NULL;
                cur_client->bytes_written = 0;
            }
            else if (cur_client->http_entry->cache_entry != NULL && cur_client->http_entry->cache_entry->is_full) {
                cur_client->status = GETTING_FROM_CACHE;
                cur_client->cache_entry = cur_client->http_entry->cache_entry;
                cur_client->http_entry->clients--;
                cur_client->http_entry = NULL;
            }
        }
        check_finished_writing_to_client(cur_client);

        FD_SET(cur_client->sock_fd, &readfds);

        if ((cur_client->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) {
            FD_SET(cur_client->sock_fd, &writefds);
        }

        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;

        if (cur_http->clients == 0) {
            if (cur_http->status == SOCK_ERROR || cur_http->status == SOCK_DONE || cur_http->status == NON_SOCK_ERROR) {
                remove_http(cur_http);
                cur_http = next;
                continue;
            }
            #ifdef DROP_HTTP_NO_CLIENTS
            if (cur_http->cache_entry != NULL && !cur_http->cache_entry->is_full) {
                cache_remove(cur_http->cache_entry, &cache);
                cur_http->cache_entry = NULL;
            }
            if (cur_http->status == DOWNLOADING || cur_http->status == AWAITING_REQUEST) {
                close(cur_http->sock_fd);
            }
            remove_http(cur_http);
            cur_http = next;
            continue;
            #endif
        }

        if (cur_http->status != SOCK_ERROR && cur_http->status != SOCK_DONE && cur_http->status != NON_SOCK_ERROR) {
            FD_SET(cur_http->sock_fd, &readfds);
        }
        if (cur_http->status == AWAITING_REQUEST) {
            FD_SET(cur_http->sock_fd, &writefds);
        }

        cur_http = next;
    }
}

void update_connections() {
    client_t *cur_client = client_list.head;
    while (cur_client != NULL) {
        client_t *next = cur_client->next;
        if (cur_client->status != SOCK_ERROR && cur_client->status != SOCK_DONE && cur_client->status != NON_SOCK_ERROR && FD_ISSET(cur_client->sock_fd, &readfds)) {
            read_data_from_client(cur_client);
        }
        if (((cur_client->status == DOWNLOADING && cur_client->http_entry->status == DOWNLOADING && cur_client->bytes_written < cur_client->http_entry->data_size) ||
            (cur_client->status == GETTING_FROM_CACHE && cur_client->bytes_written < cur_client->cache_entry->size)) && FD_ISSET(cur_client->sock_fd, &writefds)) {
            write_to_client(cur_client);
        }
        cur_client = next;
    }

    http_t *cur_http = http_list.head;
    while (cur_http != NULL) {
        http_t *next = cur_http->next;
        if (cur_http->status != SOCK_ERROR && cur_http->status != SOCK_DONE && cur_http->status != NON_SOCK_ERROR && FD_ISSET(cur_http->sock_fd, &readfds)) {
            read_http_data(cur_http);
        }
        if (cur_http->status == AWAITING_REQUEST && FD_ISSET(cur_http->sock_fd, &writefds)) {
            send_http_request(cur_http);
        }
        cur_http = next;
    }
}

void update_accept() {
    if (FD_ISSET(listen_fd, &readfds)) {
        int client_sock_fd = accept(listen_fd, NULL, NULL);
        if (client_sock_fd == -1) {
            if (ERROR_LOG) perror("update_accept: accept error");
            return;
        }
        create_client(client_sock_fd);
    }
}

int update_stdin() {
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
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
    while (TRUE) {
        init_select_masks();

        int num_fds_ready = select(select_max_fd + 1, &readfds, &writefds, NULL, NULL);
        if (num_fds_ready == -1) {
            if (ERROR_LOG) perror("proxy_spin: select error");
            break;
        }
        if (num_fds_ready == 0) continue;

        update_connections();
        update_accept();
        if (update_stdin() == -1) break;
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
    if ((listen_fd = open_listen_socket(port)) == -1) return EXIT_FAILURE;
    select_max_fd = MAX(select_max_fd, listen_fd);

    proxy_spin();

    remove_all_connections();
    cache_destroy(&cache);
    close(listen_fd);

    return EXIT_SUCCESS;
}
