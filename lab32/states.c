#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "states.h"

void print_error(const char *prefix, int code) {
    if (prefix == NULL) prefix = "error";
    char buf[256];
    if (strerror_r(code, buf, sizeof(buf)) != 0) {
        strcpy(buf, "(unable to generate error!)");
    }
    fprintf(stderr, "%s: %s\n", prefix, buf);
}

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

int read_lock_rwlock(pthread_rwlock_t *rwlock, const char *error) {
    int err_code = pthread_rwlock_rdlock(rwlock);
    if (err_code != 0) {
        if (ERROR_LOG) print_error(error, err_code);
    }
    return err_code;
}

int write_lock_rwlock(pthread_rwlock_t *rwlock, const char *error) {
    int err_code = pthread_rwlock_wrlock(rwlock);
    if (err_code != 0) {
        if (ERROR_LOG) print_error(error, err_code);
    }
    return err_code;
}

int unlock_rwlock(pthread_rwlock_t *rwlock, const char *error) {
    int err_code = pthread_rwlock_unlock(rwlock);
    if (err_code != 0) {
        if (ERROR_LOG) print_error(error, err_code);
    }
    return err_code;
}
