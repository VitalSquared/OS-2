#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <semaphore.h>

#define RATIO 20000
#define BUF_SIZE 4096
#define MAX_NUM_OF_LINES 100

#define TRUE 1
#define FALSE 0

typedef struct node {
    char *value;
    size_t length;
    struct node *next;
} node_t;

typedef struct list {
    sem_t sem;
    node_t *head;
    node_t *tail;
} list_t;

list_t list = (list_t) { .head = NULL, .tail = NULL };

int all_threads_created = FALSE;

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

int wait_sem() {
    if (sem_wait(&list.sem) == -1) {
        perror("Unable to wait semaphore");
        return -1;
    }
    return 0;
}

int post_sem() {
    if (sem_post(&list.sem) == -1) {
        perror("Unable to post semaphore");
        return -1;
    }
    return 0;
}

void list_destroy() {
    node_t *cur = list.head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free(cur->value);
        free(cur);
        cur = next;
    }
    sem_destroy(&list.sem);
}

int list_insert(node_t *node) {
    if (wait_sem() != 0) {
        return -1;
    }

    if (list.head == NULL) {
        list.head = node;
    }
    else {
        list.tail->next = node;
    }
    list.tail = node;

    return post_sem();
}

int list_print() {
    if (wait_sem() != 0) {
        return -1;
    }

    printf("--Your list--\n");
    node_t *cur = list.head;
    while (cur != NULL) {
        printf("%s", cur->value);
        cur = cur->next;
    }
    printf("--End of list--\n");

    return post_sem();
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

    node_t *node = (node_t *)param;

    while (!all_threads_created) {}

    exact_usleep(RATIO * node->length);
    if (list_insert(node) == -1) {
        free(node->value);
        free(node);
    }

    return param;
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
    if (sem_init(&list.sem, 0, 1) == -1) {
        perror("Unable to init semaphore");
        return EXIT_FAILURE;
    }

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

    node_t *nodes[MAX_NUM_OF_LINES];
    for (int i = 0; i < num_of_lines; i++) {
        nodes[i] = (node_t *)malloc(sizeof(node_t));
        if (nodes[i] == NULL) {
            perror("Unable to allocate memory for node");
            free(lines[i]);
            continue;
        }
        nodes[i]->value = lines[i];
        nodes[i]->length = strlen(lines[i]);
        nodes[i]->next = NULL;
    }

    printf("Finished strings reading. Sorting started...\n");

    pthread_t threads[num_of_lines];
    for (int i = 0; i < num_of_lines; i++) {
        if (nodes[i] == NULL) {
            continue;
        }

        int error_code = pthread_create(&threads[i], NULL, sleep_and_print, nodes[i]);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            free(lines[i]);
            free(nodes[i]);
        }
    }
    all_threads_created = TRUE;

    for (int i = 0; i < num_of_lines; i++) {
        pthread_join(threads[i], NULL);
    }

    list_print();
    list_destroy();

    return EXIT_SUCCESS;
}
