#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#define NO_ERROR 0

#define RATIO 200000
#define BUF_SIZE 4096
#define MAX_NUM_OF_STRINGS 100

#define TRUE 1
#define FALSE 0

void print_error(const char *prefix, int code) {
    if (prefix == NULL) {
        prefix = "error";
    }
    char buf[256];
    if (strerror_r(code, buf, sizeof(buf)) != 0) {
        strcpy(buf, "(unable to generate error!)");
    }
    fprintf(stderr, "%s: %s\n", prefix, buf);
}

void exact_usleep(unsigned time_left) {
    do {
        time_left = usleep(time_left);
    } while (time_left > 0);
}

void *wait_and_print(void *param) {
    if (param == NULL) {
        fprintf(stderr, "wait_and_print: invalid param\n");
        return NULL;
    }

    char *str = (char *)param;
    size_t length = strlen(str);

    exact_usleep(RATIO * length);
    write(STDOUT_FILENO, str, length);

    free(str);
    return NULL;
}

char *read_strings(int *is_eof) {
    char *str = NULL;
    size_t length = 0;

    while (1) {
        char *ptr = (char *)realloc(str, (length + BUF_SIZE) * sizeof(char));
        if (ptr == NULL) {
            perror("read_strings: Unable to (re)allocate memory for string");
            break;
        }
        str = ptr;

        errno = NO_ERROR;
        char *check = fgets(str + length, BUF_SIZE, stdin);
        if (check == NULL) {
            if (errno != NO_ERROR)
                perror("read_strings: Unable to read from stdin");
            *is_eof = TRUE;
            break;
        }

        length += strlen(check);
        if (str[length - 1] == '\n') {
            break;
        }
    }

    return str;
}

int main() {
    char *strings[MAX_NUM_OF_STRINGS];
    int num_of_strings = 0;
    int is_eof = FALSE;

    while (num_of_strings < MAX_NUM_OF_STRINGS && !is_eof) {
        strings[num_of_strings] = read_strings(&is_eof);
        if (strings[num_of_strings] == NULL)
            break;

        num_of_strings++;
    }

    for (int i = 0; i < num_of_strings; i++) {
        pthread_t thread;

        int error_code = pthread_create(&thread, NULL, wait_and_print, strings[i]);
        if (error_code != NO_ERROR) {
            print_error("Unable to create thread", error_code);
            free(strings[i]);
            continue;
        }
    }

    pthread_exit(NULL);
}
