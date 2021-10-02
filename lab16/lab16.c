#include <stdio.h>
#include <stdlib.h>
#include <semaphore.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#define NUMBER_OF_LINES 10
#define NUMBER_OF_SEMAPHORES 2

pid_t relative; //in parent process - pid of child; in child process - pid of parent;
sem_t *semaphores[NUMBER_OF_SEMAPHORES];

void print_messages(int first_sem, int second_sem, const char *message) {
    if (message == NULL) {
        fprintf(stderr, "print_messages: message was NULL\n");
        return;
    }
    size_t msg_length = strlen(message);

    for (int i = 0; i < NUMBER_OF_LINES; i++) {
        if (sem_wait(semaphores[first_sem]) == -1) {
            perror("Unable to wait semaphore");
            kill(relative, SIGKILL);
            return;
        }

        if (write(STDOUT_FILENO, message, msg_length) == -1) {
            perror("Unable to write to stdout");
        }

        if (sem_post(semaphores[second_sem]) == -1) {
            perror("Unable to post semaphore");
            kill(relative, SIGKILL);
            return;
        }
    }
}

int main() {
    int sem_init_values[NUMBER_OF_SEMAPHORES] = { 1, 0 };
    const char *sem_names[NUMBER_OF_SEMAPHORES] = { "/first", "/second" };

    //open semaphores with given init-values and names
    for (int i = 0; i < NUMBER_OF_SEMAPHORES; i++) {
        semaphores[i] = sem_open(sem_names[i], O_CREAT | O_EXCL, 0644, sem_init_values[i]);
        if (semaphores[i] == SEM_FAILED) {
            perror("Unable to init semaphore");
            for (int j = 0; j < i; j++) {
                sem_close(semaphores[j]);
                sem_unlink(sem_names[j]);
            }
            return EXIT_FAILURE;
        }
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("Unable to fork process");
        for (int i = 0; i < NUMBER_OF_SEMAPHORES; i++) {
            sem_close(semaphores[i]);
            sem_unlink(sem_names[i]);
        }
        return EXIT_FAILURE;
    }

    //execution of child process//
    if (pid == 0) {
        relative = getppid(); //get parent pid
        print_messages(1, 0, "Child\n");
        for (int i = 0; i < NUMBER_OF_SEMAPHORES; i++) {
            sem_close(semaphores[i]);
        }
        return EXIT_SUCCESS;
    }

    //execution of parent process//

    relative = pid; //get child pid from fork()
    print_messages(0, 1, "Parent\n");

    pid_t waited_pid = wait(NULL);
    if (waited_pid == -1) {
        perror("Unable to wait child process");
    }

    for (int i = 0; i < NUMBER_OF_SEMAPHORES; i++) {
        sem_close(semaphores[i]);
        sem_unlink(sem_names[i]);
    }

    return EXIT_SUCCESS;
}
