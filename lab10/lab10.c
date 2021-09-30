#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define FOOD 50
#define DELAY 30000
#define NUM_OF_PHILO 5

#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

pthread_mutex_t food_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t forks_mutex[NUM_OF_PHILO] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };

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

void lock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_lock(mutex);
    if (error_code != 0) {
        print_error("Unable to lock mutex", error_code);
        exit(EXIT_FAILURE); // if mutex fails, the logic of program is lost, should stop the whole process
    }
}

void unlock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_unlock(mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
        exit(EXIT_FAILURE); // if mutex fails, the logic of program is lost, should stop the whole process
    }
}

int get_food() {
    static int total_food = FOOD;
    int my_food;

    lock_mutex(&food_mutex);
    if (total_food > 0) {
        total_food--;
    }
    my_food = total_food;
    unlock_mutex(&food_mutex);

    return my_food;
}

void pick_fork_up(int phil, int fork, char *hand) {
    lock_mutex(&forks_mutex[fork]);
    printf("Philosopher %d: got %s fork %d\n", phil, hand, fork);
}

void put_forks_down(int left_fork, int right_fork) {
    unlock_mutex(&forks_mutex[left_fork]);  //we first put down "bigger" fork
    unlock_mutex(&forks_mutex[right_fork]); //then "smaller" fork
}

void *philosopher(void *param) {
    int id = (int)param;
    int right_fork = id;
    int left_fork = id + 1;

    // we make sure that right_fork is always less than left_work (ordered)
    if (left_fork == NUM_OF_PHILO) {
        left_fork = right_fork;
        right_fork = 0;
    }

    printf("Philosopher %d sitting down to dinner.\n", id);

    int food;
    while ((food = get_food()) > 0) {
        printf("Philosopher %d: gets food %d.\n", id, food);

        pick_fork_up(id, right_fork, "right");  // we always pick up "smaller" fork first
        pick_fork_up(id, left_fork, "left");    // only then we pick up "bigger" fork

        printf("Philosopher %d: eats.\n", id);
        usleep(DELAY * (FOOD - food + 1));

        put_forks_down(left_fork, right_fork);
    }

    printf("Philosopher %d is done eating.\n", id);
    return NULL;
}

int main() {
    int error_code;
    pthread_t threads[NUM_OF_PHILO];

    for (int i = 0; i < NUM_OF_PHILO; i++) {
        error_code = pthread_create(&threads[i], NULL, philosopher, (void *)i);
        if (error_code != 0) {
            print_error("Unable to create thread", error_code);
        }
    }

    pthread_exit(NULL);
}
