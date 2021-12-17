#include "url_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#define BUF_SIZE 4096

char *copy_substring(const char *string, regmatch_t matches[], int match_num) {
    if (matches[match_num].rm_so == -1 || matches[match_num].rm_so == matches[match_num].rm_eo)
        return NULL;

    regoff_t length = matches[match_num].rm_eo - matches[match_num].rm_so;

    char *copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        perror("url parser: Unable to allocate memory for url cache_entry");
        return NULL;
    }

    memcpy(copy, string + matches[match_num].rm_so, length);
    copy[length] = '\0';
    return copy;
}

void print_reg_error(regex_t *regexp, int err_code) {
    char err_msg[256];
    regerror(err_code, regexp, err_msg, 256);
    fprintf(stderr, "url parser: reg error: %s\n", err_msg);
}

url_t *parse_url(const char *url_string, int default_port) {
    const char *scheme = "([a-zA-Z][-a-zA-Z0-9+.]*):";    //1 - scheme
    const char *authority = "(//((.*)@)?([-a-zA-Z0-9.]*)(:([0-9]+))?)?";   //2 - authority outer, 3 - user outer, 4 - user, 5 - host, 6 - port outer, 7 - port

    char pattern[BUF_SIZE];
    sprintf(pattern, "^%s%s(.*)?$", scheme, authority); //8 - everything else

    regex_t regexp;
    regmatch_t matches[8];

    int err_code = regcomp(&regexp, pattern, REG_EXTENDED);
    if (err_code != 0) {
        print_reg_error(&regexp, err_code);
        return NULL;
    }

    err_code = regexec(&regexp, url_string, 8, matches, 0);
    if (err_code != 0) {
        print_reg_error(&regexp, err_code);
        regfree(&regexp);
        return NULL;
    }

    regfree(&regexp);

    url_t *url = (url_t *)malloc(sizeof(url_t));
    if (url == NULL) {
        perror("url_parser: Unable to allocate memory for url");
        return NULL;
    }

    url->full = strdup(url_string);
    url->scheme = copy_substring(url_string, matches, 1);
    url->user = copy_substring(url_string, matches, 4);
    url->hostname = copy_substring(url_string, matches, 5);
    char *port_string = copy_substring(url_string, matches, 7);
    url->port = (port_string == NULL) ? default_port : atoi(port_string);

    return url;
}

void free_url(url_t *url) {
    free(url->full);
    free(url->scheme);
    free(url->user);
    free(url->hostname);
    free(url);
}
