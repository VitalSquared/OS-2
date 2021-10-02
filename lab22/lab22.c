#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define FOOD 50
#define DELAY 30000
#define NUM_OF_PHILO 5

#define IS_STRING_EMPTY(STR) ((STR) == NULL || (STR)[0] == '\0')

#define LOCKED 0
#define NOT_LOCKED (-1)

pthread_cond_t all_forks_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t all_forks_mutex = PTHREAD_MUTEX_INITIALIZER;
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

int try_lock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_trylock(mutex);
    if (error_code != 0) {
        if (error_code == EBUSY) {  //EBUSY - mutex is already locked by other thread
            return NOT_LOCKED;  //we return special constant if mutex wasn't able to be locked
        }
        print_error("Unable to try-lock mutex", error_code);
        exit(EXIT_FAILURE); // if mutex fails with error NOT EBUSY, the logic of program is lost, should stop the whole process
    }
    return LOCKED;  //we return special constant if mutex was locked
}

void unlock_mutex(pthread_mutex_t *mutex) {
    int error_code = pthread_mutex_unlock(mutex);
    if (error_code != 0) {
        print_error("Unable to unlock mutex", error_code);
        exit(EXIT_FAILURE); // if mutex fails, the logic of program is lost, should stop the whole process
    }
}

void wait_cond(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int error_code = pthread_cond_wait(cond, mutex);
    if (error_code != 0) {
        print_error("Unable to wait on cond variable", error_code);
        exit(EXIT_FAILURE); // if we can't wait on cond var, the logic of program is lost, should stop the whole process
    }
}

void broadcast_cond(pthread_cond_t *cond) {
    int error_code = pthread_cond_broadcast(cond);
    if (error_code != 0) {
        print_error("Unable to broadcast cond variable", error_code);
        exit(EXIT_FAILURE); // if we can't broadcast cond var, the logic of program is lost, should stop the whole process
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

void pick_forks_up(int phil, int left_fork, int right_fork) {
    lock_mutex(&all_forks_mutex);

    while (1) {
        int lock1 = try_lock_mutex(&forks_mutex[right_fork]);
        if (lock1 == LOCKED) {
            int lock2 = try_lock_mutex(&forks_mutex[left_fork]);
            if (lock2 == LOCKED) {
                break;  //both mutexes are locked, break from while-loop
            }

            unlock_mutex(&forks_mutex[right_fork]); //left_fork couldn't be locked, we unlock already-locked right_fork
        }

        wait_cond(&all_forks_cond, &all_forks_mutex); //wait until other philosophers put down forks and wake us up
    }

    unlock_mutex(&all_forks_mutex);
    printf("Philosopher %d: got forks %d and %d.\n", phil, left_fork, right_fork);
}

void put_forks_down(int left_fork, int right_fork) {
    lock_mutex(&all_forks_mutex);

    unlock_mutex(&forks_mutex[left_fork]);
    unlock_mutex(&forks_mutex[right_fork]);

    broadcast_cond(&all_forks_cond);    //wake up philosophers waiting on cond var

    unlock_mutex(&all_forks_mutex);
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

        pick_forks_up(id, left_fork, right_fork);

        printf("Philosopher %d: eats.\n", id);
        usleep(DELAY * (FOOD - food + 1));

        put_forks_down(left_fork, right_fork);
    }

    printf("Philosopher %d is done eating.\n", id);
    return param;
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
