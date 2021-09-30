#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#define RATIO 200000
#define BUF_SIZE 4096
#define MAX_NUM_OF_LINES 100

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

void *sleep_and_print(void *param) {
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

char *read_line(int *is_eof) {
    char *str = NULL;
    size_t length = 0;

    while (1) {
        char *ptr = (char *)realloc(str, (length + BUF_SIZE) * sizeof(char));
        if (ptr == NULL) {
            perror("read_strings: Unable to (re)allocate memory for string");
            break;
        }
        str = ptr; // realloc may return different pointer, so we need to update str

        errno = 0;  //errno is unique to calling thread (furthermore, we call this function before spawning any threads)
        char *check = fgets(str + length, BUF_SIZE, stdin);
        if (check == NULL) {
            if (errno != 0) {
                perror("read_strings: Unable to read from stdin");
            }
            *is_eof = TRUE;
            break;
        }

        length += strlen(check); // 'check' will be null-terminated
        if (str[length - 1] == '\n') {
            break;
        }
    }

    return str;
}

int main() {
    int is_eof = FALSE;
    int num_of_lines = 0;
    char *lines[MAX_NUM_OF_LINES];

    while (num_of_lines < MAX_NUM_OF_LINES && !is_eof) {
        lines[num_of_lines] = read_line(&is_eof);
        if (lines[num_of_lines] == NULL) {
            break;
        }
        num_of_lines++;
    }

    for (int i = 0; i < num_of_lines; i++) {
        pthread_t thread;
        int error_code = pthread_create(&thread, NULL, sleep_and_print, lines[i]);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            free(lines[i]);
        }
    }

    pthread_exit(NULL);
}
