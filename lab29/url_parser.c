#include "url_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#define NO_FLAGS 0
#define REG_SUCCESS 0
#define ERR_BUF_SIZE 256

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
    char err_msg[ERR_BUF_SIZE];
    regerror(err_code, regexp, err_msg, ERR_BUF_SIZE);
    fprintf(stderr, "url parser: reg error: %s\n", err_msg);
}

url_t *parse_url(const char *url_string, int default_port) {
    const char *pattern = "^([a-z]+)://((.*)@)?([a-zA-Z][-a-zA-Z0-9.]*)(:([0-9]+))?(.*)$";
    //                      /--1---/   /-2,3-/ /----------4-----------//---5,6---/ /-7-/
    //                      protocol    user           hostname            port    uri
    static regex_t regexp;
    regmatch_t matches[10];

    int err_code = regcomp(&regexp, pattern, REG_EXTENDED);
    if (err_code != REG_SUCCESS) {
        print_reg_error(&regexp, err_code);
        return NULL;
    }

    err_code = regexec(&regexp, url_string, 10, matches, NO_FLAGS);
    if (err_code != REG_SUCCESS) {
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

    url->full = (char *)malloc(strlen(url_string) + 1);
    if (url->full != NULL)
        strcpy(url->full, url_string);

    url->protocol = copy_substring(url_string, matches, 1);
    url->user = copy_substring(url_string, matches, 3);
    url->hostname = copy_substring(url_string, matches, 4);

    char *port_string = copy_substring(url_string, matches, 6);
    if (port_string == NULL)
        url->port = default_port;
    else
        url->port = atoi(port_string);

    return url;
}

void free_url(url_t *url) {
    free(url->full);
    free(url->hostname);
    free(url->user);
    free(url->protocol);
    free(url);
}
