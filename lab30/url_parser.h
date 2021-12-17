typedef struct url {
    char *full;
    char *scheme;
    char *user;
    char *hostname;
    int port;
} url_t;

url_t *parse_url(const char *url_string, int default_port);
void free_url(url_t *url);
