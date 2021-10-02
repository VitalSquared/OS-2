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

typedef struct node {
    char *value;
    struct node *next;
} node_t;

typedef struct list {
    pthread_mutex_t mutex;
    node_t *head;
    node_t *tail;
} list_t;

list_t *global_list = NULL;

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

int lock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_lock(mutex);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
    }
    return error_code;
}

int unlock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_unlock(mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
    }
    return error_code;
}

list_t *list_create() {
    list_t *lst = (list_t *)malloc(sizeof(list_t));
    if (lst == NULL) {
        perror("Unable to allocate memory for list");
        return NULL;
    }

    lst->head = NULL;
    lst->tail = NULL;
    int error_code = pthread_mutex_init(&lst->mutex, NULL);
    if (error_code != 0) {
        print_error("Unable to init list mutex", error_code);
        free(lst);
        return NULL;
    }

    return lst;
}

void free_node(node_t *node) {
    if (node == NULL) {
        return;
    }
    free(node->value);
    free(node);
}

void list_destroy(list_t *lst) {
    if (lst == NULL) {
        return;
    }

    node_t *cur = lst->head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free_node(cur);
        cur = next;
    }

    int error_code = pthread_mutex_destroy(&lst->mutex);
    if (error_code != 0) {
        print_error("Unable to destroy mutex", error_code);
    }

    free(lst);
}

int list_insert(list_t *lst, char *str) {
    if (lst == NULL || str == NULL) {
        return 0;
    }

    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (new_node == NULL) {
        perror("Unable to allocate memory for node");
        return -1;
    }
    new_node->value = str;
    new_node->next = NULL;

    if (lock_mutex(&lst->mutex) != 0) {
        free_node(new_node);
        return -1;
    }

    if (lst->head == NULL) {
        lst->head = new_node;
        lst->tail = new_node;
    }
    else {
        lst->tail->next = new_node;
        lst->tail = new_node;
    }

    return unlock_mutex(&lst->mutex) == 0 ? 0 : -1;
}

int list_print(list_t *lst) {
    if (lst == NULL) {
        printf("--List is NULL--\n");
        return 0;
    }

    if (lock_mutex(&lst->mutex) != 0) {
        return -1;
    }

    printf("--Your list--\n");
    node_t *cur = lst->head;
    while (cur != NULL) {
        printf("%s", cur->value);
        cur = cur->next;
    }
    printf("--End of list--\n");

    return unlock_mutex(&lst->mutex) == 0 ? 0 : -1;
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
    list_insert(global_list, str);

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

    global_list = list_create();
    if (global_list == NULL) {
        return EXIT_FAILURE;
    }

    while (num_of_lines < MAX_NUM_OF_LINES && !is_eof) {
        lines[num_of_lines] = read_line(&is_eof);
        if (lines[num_of_lines] == NULL) {
            break;
        }
        num_of_lines++;
    }

    pthread_t threads[num_of_lines];
    for (int i = 0; i < num_of_lines; i++) {
        int error_code = pthread_create(&threads[i], NULL, sleep_and_print, lines[i]);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
            free(lines[i]);
        }
    }

    for (int i = 0; i < num_of_lines; i++) {
        pthread_join(threads[i], NULL);
    }

    list_print(global_list);
    list_destroy(global_list);

    return EXIT_SUCCESS;
}
