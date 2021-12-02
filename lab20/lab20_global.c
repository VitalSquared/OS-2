#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 80
#define SORT_INTERVAL_SECONDS 5

#define STR_ORDERED(STR1, STR2) (strcmp(STR1, STR2) <= 0)

typedef struct node {
    char *value;
    struct node *next;
} node_t;

typedef struct list {
    pthread_rwlock_t rwlock;
    node_t *head;
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

int read_lock_rwlock(pthread_rwlock_t *rwlock) {
    int error_code = pthread_rwlock_rdlock(rwlock);
    if (error_code != 0) {
        print_error("Unable to read-lock rwlock", error_code);
    }
    return error_code;
}

int write_lock_rwlock(pthread_rwlock_t *rwlock) {
    int error_code = pthread_rwlock_wrlock(rwlock);
    if (error_code != 0) {
        print_error("Unable to write-lock rwlock", error_code);
    }
    return error_code;
}

int unlock_rwlock(pthread_rwlock_t *rwlock) {
    int error_code = pthread_rwlock_unlock(rwlock);
    if (error_code != 0) {
        print_error("Unable to unlock rwlock", error_code);
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
    int error_code = pthread_rwlock_init(&lst->rwlock, NULL);
    if (error_code != 0) {
        free(lst);
        print_error("Unable to init rwlock", error_code);
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

    write_lock_rwlock(&lst->rwlock);

    node_t *cur = lst->head;
    node_t *next = NULL;
    while (cur != NULL) {
        next = cur->next;
        free_node(cur);
        cur = next;
    }

    unlock_rwlock(&lst->rwlock);

    int error_code = pthread_rwlock_destroy(&lst->rwlock);
    if (error_code != 0) {
        print_error("Unable to destroy rwlock", error_code);
    }

    free(lst);
}

int list_insert(list_t *lst, const char *str) {
    if (lst == NULL) {
        return 0;
    }

    node_t *new_node = (node_t *)malloc(sizeof(node_t));
    if (new_node == NULL) {
        perror("Unable to allocate memory for node");
        return -1;
    }

    new_node->value = strdup(str);
    if (new_node->value == NULL) {
        perror("Unable to allocate memory for node value");
        free(new_node);
        return -1;
    }

    if (write_lock_rwlock(&lst->rwlock) != 0) {
        free_node(new_node);
        return -1;
    }

    new_node->next = lst->head;
    lst->head = new_node;

    return unlock_rwlock(&lst->rwlock) == 0 ? 0 : -1;
}

int list_print(list_t *lst) {
    if (lst == NULL) {
        printf("--List is NULL--\n");
        return 0;
    }

    if (read_lock_rwlock(&lst->rwlock) != 0) {
        return -1;
    }

    printf("--Your list--\n");
    node_t *cur = lst->head;
    while (cur != NULL) {
        printf("%s\n", cur->value);
        cur = cur->next;
    }
    printf("--End of list--\n");

    return unlock_rwlock(&lst->rwlock) == 0 ? 0 : -1;
}

void swap(node_t *a, node_t *b) {
    char *tmp = a->value;
    a->value = b->value;
    b->value = tmp;
}

int list_sort(list_t *lst) {
    if (write_lock_rwlock(&lst->rwlock) != 0) {
        return -1;
    }

    for (node_t *node_i = lst->head; node_i != NULL; node_i = node_i->next) {
        for (node_t *node_j = node_i->next; node_j != NULL; node_j = node_j->next) {
            if (!STR_ORDERED(node_i->value, node_j->value)) {
                swap(node_i, node_j);
            }
        }
    }

    return unlock_rwlock(&lst->rwlock) == 0 ? 0 : -1;
}

void *interval_list_sorting(void *param) {
    while (global_list != NULL) {
        sleep(SORT_INTERVAL_SECONDS);

        if (list_sort(global_list) == -1) {
            fprintf(stderr, "Sorting thread encountered error. Sorting stopped.\n");
            break;
        }
    }
    return NULL;
}

int get_strings() {
    if (global_list == NULL) {
        fprintf(stderr, "get_strings: global_list is NULL\n");
        return -1;
    }

    char buf[BUF_SIZE + 1];
    while (1) {
        ssize_t bytes_read = read(STDIN_FILENO, buf, BUF_SIZE);
        if (bytes_read == -1) {
            perror("Unable to read from stdin");
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        buf[bytes_read] = '\0';
        if (buf[bytes_read - 1] == '\n') {
            buf[bytes_read - 1] = '\0';
            bytes_read--;
        }

        if (bytes_read == 0) {
            if (list_print(global_list) == -1) {
                return -1;
            }
            continue;
        }

        if (list_insert(global_list, buf) == -1) {
            return -1;
        }
    }

    return 0;
}

int main() {
    global_list = list_create();

    pthread_t thread;
    int error_code = pthread_create(&thread, NULL, interval_list_sorting, NULL);
    if (error_code != 0) {
        print_error("Unable to create thread", error_code);
        list_destroy(global_list);
        return EXIT_FAILURE;
    }

    get_strings();

    pthread_cancel(thread);
    pthread_join(thread, NULL);

    list_destroy(global_list);
    global_list = NULL;

    pthread_exit(NULL);
}
