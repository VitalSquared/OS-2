
#define URL_PORT_ERROR 0

typedef struct url {
    char *full;
    char *protocol;
    char *user;
    int port;
    char *hostname;
} url_t;

url_t *parse_url(const char *url_string, int default_port);
void free_url(url_t *url);
