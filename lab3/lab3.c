#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NO_ERROR 0
#define ERROR_BUF_SIZE 256

#define NUM_THREADS 4

#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

typedef struct lines_container {
    char **lines;
} lines_container_t;

void print_error(const char *prefix_message, int error_code) {
    char error_message[ERROR_BUF_SIZE];
    strerror_r(error_code, error_message, ERROR_BUF_SIZE);
    if (!IS_STRING_EMPTY(prefix_message))
        fprintf(stderr, "%s: ", prefix_message);
    fprintf(stderr, "%s\n", error_message);
}

void wrap_lines(lines_container_t *container, char **lines) {
    container->lines = lines;
}

void *print_lines(void *param) {
    lines_container_t *container = (lines_container_t *)param;
    if (container == NULL || container->lines == NULL) {
        fprintf(stderr, "print_lines: invalid param\n");
        return NULL;
    }

    for (int i = 0; container->lines[i] != NULL; i++)
        printf("%s\n", container->lines[i]);

    return NULL;
}

int create_threads(pthread_t *threads, lines_container_t *containers, size_t size) {
    int error_code;
    int number_of_threads_created = 0;

    for (size_t i = 0; i < size; i++) {
        error_code = pthread_create(&threads[i], NULL, print_lines, &containers[i]);
        if (error_code != NO_ERROR) {
            print_error("Unable to create thread", error_code);
            break;
        }
        number_of_threads_created++;
    }

    return number_of_threads_created;
}

int join_threads(pthread_t *threads, size_t size) {
    int error_code;
    int return_value = NO_ERROR;

    for (int i = 0; i < size; i++) {
        error_code = pthread_join(threads[i], NULL);
        if (error_code != NO_ERROR) {
            print_error("Unable to join thread", error_code);
            return_value = error_code;
        }
    }

    return return_value;
}

int main() {
    pthread_t threads[NUM_THREADS];
    lines_container_t containers[NUM_THREADS];

    wrap_lines(&containers[0], (char *[]){ "[1] line 1", "[1] line 2", "[1] line 3", NULL });
    wrap_lines(&containers[1], (char *[]){ "[2] line 1", "[2] line 2", "[2] line 3", "[2] line 4", NULL });
    wrap_lines(&containers[2], (char *[]){ "[3] line 1", "[3] line 2", "[3] line 3", NULL });
    wrap_lines(&containers[3], (char *[]){ "[4] line 1", "[4] line 2", NULL });

    int exit_status = EXIT_SUCCESS;

    int number_of_threads_created = create_threads(threads, containers, NUM_THREADS);
    if (number_of_threads_created < NUM_THREADS)
        exit_status = EXIT_FAILURE;

    if (join_threads(threads, number_of_threads_created) != NO_ERROR)
        exit_status = EXIT_FAILURE;

    return exit_status;
}
