#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#define NO_ERROR 0
#define WRITE_ERROR (-1)
#define ERROR_BUF_SIZE 256

#define CHILD_THREAD_TEXT "Child\n"

#define NOT_CANCELLED 1
#define WAIT_TIME_SECONDS 2

#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

void print_error(const char *prefix_message, int error_code) {
    char error_message[ERROR_BUF_SIZE];
    strerror_r(error_code, error_message, ERROR_BUF_SIZE);
    if (!IS_STRING_EMPTY(prefix_message))
        fprintf(stderr, "%s: ", prefix_message);
    fprintf(stderr, "%s\n", error_message);
}

void *print_lines(void *param) {
    if (param == NULL) {
        fprintf(stderr, "print_lines: invalid param\n");
        return NULL;
    }

    char *line = (char *)param;
    size_t length = strlen(line);

    while (NOT_CANCELLED) {
        ssize_t bytes_written = write(STDOUT_FILENO, line, length);
        if (bytes_written == WRITE_ERROR) {
            perror("Unable to write to stdout");
            break;
        }
    }

    return NULL;
}

void exact_sleep(unsigned int seconds) {
    unsigned int time_left = seconds;
    do {
        time_left = sleep(time_left);
    } while (time_left > 0);
}

int main() {
    pthread_t thread;

    int error_code = pthread_create(&thread, NULL, print_lines, CHILD_THREAD_TEXT);
    if (error_code != NO_ERROR) {
        print_error("Unable to create thread", error_code);
        return EXIT_FAILURE;
    }

    exact_sleep(WAIT_TIME_SECONDS);

	int exit_status = EXIT_FAILURE;

    error_code = pthread_cancel(thread);
	if (error_code != NO_ERROR) {
        print_error("Unable to join thread", error_code);
        exit_status = EXIT_FAILURE;
    }

    error_code = pthread_join(thread, NULL);
    if (error_code != NO_ERROR) {
        print_error("Unable to join thread", error_code);
        exit_status = EXIT_FAILURE;
    }

    return exit_status;
}
