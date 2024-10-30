#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <semaphore.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define MAX_PROCESSES 100
#define MAX_PROGRAM_NAME 256

int NCPU;
int TSLICE;
int shmid;

sem_t scheduler_sem; // Semaphores for scheduling
sem_t print_sem;
sem_t scheduler_queue_sem;

struct ProcessQueue* scheduler_queue;
struct TerminatedQueue* terminated_queue;

struct Process {
    pid_t pid;
    char command[MAX_PROGRAM_NAME];
    int state; // 0: Running, 1: Waiting
    int priority; // Priority of the process
    struct timeval start_time;
    struct timeval end_time;   
    long long total_execution_time;
    long long waiting_time;
};

struct ProcessQueue {
    struct Process processes[MAX_PROCESSES];
    int rear;
};

struct TerminatedQueue {
    struct Process processes[MAX_PROCESSES];
    int rear;
};

void enqueue(struct ProcessQueue* queue, struct Process process) {
    if (queue->rear == MAX_PROCESSES - 1) {
        printf("Queue is full.\n");
        return;
    }
    queue->processes[queue->rear] = process;
    queue->rear++;
}

void handleSIGUSR1(int signo) { 
    sem_post(&scheduler_sem);
}

void printTerminatedQueue(struct TerminatedQueue* queue) {
    for (int i = 0; i <= queue->rear; i++) {
        int waiting_time = ((queue->processes[i].end_time.tv_sec - queue->processes[i].start_time.tv_sec) * 1000) +
                           ((queue->processes[i].end_time.tv_usec - queue->processes[i].start_time.tv_usec) / 1000);
        printf("Terminated Process with PID %d. Execution Time: %lld ms and %lld ms waiting time\n",
               queue->processes[i].pid, queue->processes[i].total_execution_time, queue->processes[i].waiting_time);
    }
}

void handleSIGCHLD(int signo) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        sem_wait(&scheduler_queue_sem);

        for (int i = 0; i <= scheduler_queue->rear; i++) {
            if (scheduler_queue->processes[i].pid == pid) {
                scheduler_queue->processes[i].state = -1; // Set state to finished
                gettimeofday(&scheduler_queue->processes[i].end_time, NULL);
                struct timeval elapsedTime;
                elapsedTime.tv_sec = scheduler_queue->processes[i].end_time.tv_sec - scheduler_queue->processes[i].start_time.tv_sec;
                elapsedTime.tv_usec = scheduler_queue->processes[i].end_time.tv_usec - scheduler_queue->processes[i].start_time.tv_usec;
                long long elapsed = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
                scheduler_queue->processes[i].total_execution_time += elapsed;

                terminated_queue->processes[terminated_queue->rear] = scheduler_queue->processes[i];
                terminated_queue->rear++;
                
                for (int j = i; j < scheduler_queue->rear; j++) {
                    scheduler_queue->processes[j] = scheduler_queue->processes[j + 1];
                }
                scheduler_queue->rear--;
                break;
            }
        }
        sem_post(&scheduler_queue_sem);
    }
}

void handleSIGINT(int signo) {
    printTerminatedQueue(terminated_queue);
    exit(1);
}

void sortQueueByPriority(struct ProcessQueue* queue) {
    for (int i = 0; i < queue->rear; i++) {
        for (int j = i + 1; j < queue->rear; j++) {
            if (queue->processes[i].priority < queue->processes[j].priority) {
                struct Process temp = queue->processes[i];
                queue->processes[i] = queue->processes[j];
                queue->processes[j] = temp;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        perror("please provide all arguments!");
        exit(0);
    }

    sem_init(&scheduler_sem, 0, 0);
    sem_init(&print_sem, 0, 1);
    sem_init(&scheduler_queue_sem, 0, 1);

    signal(SIGCHLD, handleSIGCHLD);
    NCPU = atoi(argv[1]);
    TSLICE = atoi(argv[2]);

    signal(SIGUSR1, handleSIGUSR1);
    signal(SIGINT, handleSIGINT);

    shmid = shmget(IPC_PRIVATE, sizeof(struct ProcessQueue), 0666 | IPC_CREAT);
    if (shmid < 0) {
        perror("shmget");
        exit(1);
    }
    scheduler_queue = shmat(shmid, NULL, 0);
    if (scheduler_queue == (void*) -1) {
        perror("shmat");
        exit(1);
    }
    scheduler_queue->rear = 0;

    int terminated_shmid = shmget(IPC_PRIVATE, sizeof(struct TerminatedQueue), 0666 | IPC_CREAT);
    if (terminated_shmid < 0) {
        perror("shmget for terminated queue");
        exit(1);
    }
    terminated_queue = shmat(terminated_shmid, NULL, 0);
    if (terminated_queue == (void*) -1) {
        perror("shmat for terminated queue");
        exit(1);
    }
    terminated_queue->rear = 0;

    pid_t scheduler_pid = fork();
    if (scheduler_pid == 0) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = TSLICE * 1000000;

        while (1) {
            if (scheduler_queue->rear > 0) {
                sortQueueByPriority(scheduler_queue); // Sort by priority before scheduling

                int processesToStart = NCPU;
                for (int i = 0; i < scheduler_queue->rear && processesToStart > 0; i++) {
                    if (scheduler_queue->processes[i].state == 1) {
                        gettimeofday(&scheduler_queue->processes[i].start_time, NULL);
                        kill(scheduler_queue->processes[i].pid, SIGCONT);
                        scheduler_queue->processes[i].state = 0; // Set state to running
                        processesToStart--;
                    }
                }
            }
            usleep(TSLICE * 1000); // Sleep for TSLICE
            // Stop running processes
            for (int j = 0; j < scheduler_queue->rear; j++) {
                if (scheduler_queue->processes[j].state == 0) {
                    kill(scheduler_queue->processes[j].pid, SIGSTOP);
                    gettimeofday(&scheduler_queue->processes[j].end_time, NULL);
                    struct timeval elapsedTime;
                    elapsedTime.tv_sec = scheduler_queue->processes[j].end_time.tv_sec - scheduler_queue->processes[j].start_time.tv_sec;
                    elapsedTime.tv_usec = scheduler_queue->processes[j].end_time.tv_usec - scheduler_queue->processes[j].start_time.tv_usec;
                    long long elapsed = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
                    scheduler_queue->processes[j].total_execution_time += elapsed;
                    scheduler_queue->processes[j].state = 1; // Set state to waiting
                }
            }
        }
    } else {
        while (1) {
            char command[MAX_PROGRAM_NAME];
            printf("\nSimpleShell$ ");
            scanf("%s", command);

            if (strcmp(command, "exit") == 0) {
                break;
            } else if (strcmp(command, "submit") == 0) {
                // User submits a program and its priority
                char program[MAX_PROGRAM_NAME];
                int priority;
                scanf("%s %d", program, &priority);

                pid_t child_pid = fork();
                if (child_pid == 0) {
                    // Child process
                    usleep(TSLICE * 1000);
                    execlp(program, program, NULL);
                    perror("Execution failed");
                    exit(1);
                } else {
                    // Parent process (shell)
                    struct Process new_process;
                    new_process.pid = child_pid;
                    strcpy(new_process.command, program);
                    new_process.state = 1; // Set state to waiting
                    new_process.priority = priority; // Set process priority
                    new_process.total_execution_time = 0; // Set initial execution time = 0
                    new_process.waiting_time = TSLICE;

                    enqueue(scheduler_queue, new_process);
                    kill(scheduler_pid, SIGUSR1);
                }
            } else {
                system(command);
            }
        }

        while (terminated_queue->rear > 0) {
            printTerminatedQueue(terminated_queue);
            wait(NULL); // Wait for any child process to terminate
        }
        exit(0);
    }

    // Clean up shared memory
    shmdt(scheduler_queue);
    shmctl(shmid, IPC_RMID, NULL);
    shmdt(terminated_queue);
    shmctl(terminated_shmid, IPC_RMID, NULL);

    // Destroy semaphores
    sem_destroy(&scheduler_sem);
    sem_destroy(&print_sem);
    sem_destroy(&scheduler_queue_sem);
}
