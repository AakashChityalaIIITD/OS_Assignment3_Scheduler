#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <semaphore.h> 
#include <sys/ipc.h>
#include <sys/shm.h>

#define MAX_PROCESSES 100
#define MAX_PROGRAM_NAME 256

int NCPU;
int TSLICE;
int shmid; 

// Semaphores for scheduling
sem_t scheduler_sem; 
sem_t print_sem;
sem_t scheduler_queue_sem; 

struct ProcessQueue* scheduler_queue;
struct ProcessQueue shell_queue;
struct TerminatedQueue* terminated_queue;

struct Process {
    pid_t pid;
    char command[MAX_PROGRAM_NAME];
    int state; // 0: Running, 1: Waiting
    struct timeval start_time;
    struct timeval end_time;   
    struct timeval submission_time;  
    long long total_execution_time;
    long long waiting_time;
    long long completion_time;    
};

struct ProcessQueue {
    struct Process processes[MAX_PROCESSES];
    int rear;
};

struct TerminatedQueue {
    struct Process processes[MAX_PROCESSES];
    int rear;
};

// Helper function to round up to next TSLICE multiple
long long roundToNextTimeSlice(long long time, int tslice) {
    if (time % tslice == 0) {
        return time;
    }
    return time + (tslice - (time % tslice));
}

void enqueue(struct ProcessQueue* queue, struct Process process) {
    if (queue->rear == MAX_PROCESSES - 1) {
        printf("Queue is full.\n");
        return;
    }
    queue->processes[queue->rear] = process;
}

// Signal the scheduler to wake up
void handleSIGUSR1(int signo) {
    sem_post(&scheduler_sem);
}

void printTerminatedQueue(struct TerminatedQueue* queue) {
    for (int i = 0; i <= queue->rear; i++) {
        printf("Terminated Process with PID %d:\n", queue->processes[i].pid);
        printf("  Execution Time: %lld ms\n", 
               queue->processes[i].total_execution_time);
        printf("  Waiting Time: %lld ms\n", 
               queue->processes[i].waiting_time);
        printf("  Completion Time: %lld ms\n", 
               queue->processes[i].completion_time);
        //printf("--------------------------\n");
    }
}

// Signal handler for child process completion
void handleSIGCHLD(int signo) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        sem_wait(&scheduler_queue_sem);

        for (int i = 0; i <= scheduler_queue->rear; i++) {
            if (scheduler_queue->processes[i].pid == pid) {
                scheduler_queue->processes[i].state = -1;
                gettimeofday(&scheduler_queue->processes[i].end_time, NULL);
                
                // Calculate execution time
                struct timeval elapsedTime;
                elapsedTime.tv_sec = scheduler_queue->processes[i].end_time.tv_sec - scheduler_queue->processes[i].start_time.tv_sec;
                elapsedTime.tv_usec = scheduler_queue->processes[i].end_time.tv_usec - scheduler_queue->processes[i].start_time.tv_usec;
                long long elapsed = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
                scheduler_queue->processes[i].total_execution_time += elapsed;
                scheduler_queue->processes[i].waiting_time += (scheduler_queue->rear) * TSLICE;

                // Round up all times to next TSLICE multiple
                scheduler_queue->processes[i].total_execution_time = 
                    roundToNextTimeSlice(scheduler_queue->processes[i].total_execution_time, TSLICE);
                scheduler_queue->processes[i].waiting_time = 
                    roundToNextTimeSlice(scheduler_queue->processes[i].waiting_time, TSLICE);
                
                // Calculate completion time as sum of rounded waiting and execution time
                scheduler_queue->processes[i].completion_time = 
                    scheduler_queue->processes[i].total_execution_time + 
                    scheduler_queue->processes[i].waiting_time;

                terminated_queue->rear++;
                terminated_queue->processes[terminated_queue->rear] = scheduler_queue->processes[i];
                
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

    signal(SIGUSR1, (void (*)(int)) handleSIGUSR1);
    signal(SIGINT, (void (*)(int)) handleSIGINT);

// Create shared memory for the process queue
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
    scheduler_queue->rear = -1;

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
    terminated_queue->rear = -1;

    pid_t scheduler_pid = fork();
    if (scheduler_pid == 0) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = TSLICE * 1000000;
        int i = 0;

        while (1) {
            if (i > scheduler_queue->rear || i >= MAX_PROCESSES) {
                i = 0;
            }

            int processesToStart = NCPU;
            for (; i <= scheduler_queue->rear && processesToStart > 0; i++) {
                if (scheduler_queue->processes[i].state == 1) {
                    sem_wait(&print_sem);
                    sem_post(&print_sem);
                    gettimeofday(&scheduler_queue->processes[i].start_time, NULL);
                    kill(scheduler_queue->processes[i].pid, SIGCONT);
                    scheduler_queue->processes[i].state = 0;
                    processesToStart--;
                }
            }
            
            usleep(TSLICE * 1000);

            for (int j = 0; j <= scheduler_queue->rear; j++) {
                if (scheduler_queue->processes[j].state == 0) {
                    sem_wait(&print_sem);
                    sem_post(&print_sem);
                    kill(scheduler_queue->processes[j].pid, SIGSTOP);
                    gettimeofday(&scheduler_queue->processes[j].end_time, NULL);
                    struct timeval elapsedTime;
                    elapsedTime.tv_sec = scheduler_queue->processes[j].end_time.tv_sec - scheduler_queue->processes[j].start_time.tv_sec;
                    elapsedTime.tv_usec = scheduler_queue->processes[j].end_time.tv_usec - scheduler_queue->processes[j].start_time.tv_usec;
                    long long elapsed = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
                    
                    // Round up execution time to next TSLICE multiple
                    long long rounded_elapsed = roundToNextTimeSlice(elapsed, TSLICE);
                    scheduler_queue->processes[j].total_execution_time += rounded_elapsed;
                    
                    // Update waiting time for all waiting processes
                    for (int k = 0; k <= scheduler_queue->rear; k++) {
                        if (k != j && scheduler_queue->processes[k].state == 1) {
                            scheduler_queue->processes[k].waiting_time += TSLICE;
                        }
                    }
                    
                    scheduler_queue->processes[j].state = 1;
                }
            }
        }
    } else {
        // Parent process (SimpleShell)
        shell_queue.rear = -1;

        while (1) {
            char command[MAX_PROGRAM_NAME];
            printf("\nSimpleShell$ ");
            scanf("%s", command);

            if (strcmp(command, "exit") == 0) {
                break;
            } else if (strcmp(command, "submit") == 0) {
                char program[MAX_PROGRAM_NAME];
                scanf("%s", program);

                pid_t child_pid = fork();
                if (child_pid == 0) {
                    usleep(TSLICE * 1000);
                    execlp(program, program, NULL);
                    perror("Execution failed");
                    exit(1);
                } else {
                    struct Process new_process;
                    new_process.pid = child_pid;
                    strcpy(new_process.command, program);
                    new_process.state = 1;
                    new_process.total_execution_time = 0;
                    new_process.waiting_time = TSLICE;
                    gettimeofday(&new_process.submission_time, NULL);
                    new_process.completion_time = 0;
                    
                    scheduler_queue->rear++;
                    enqueue(scheduler_queue, new_process);

                    kill(scheduler_pid, SIGUSR1);
                }
            } else {
                system(command);
            }
        }

        // Wait for child processes to complete
        while (shell_queue.rear >= 0) {
            int status;
            pid_t pid = wait(&status);
            if (pid == -1) {
                break;
            }
            for (int i = 0; i <= shell_queue.rear; i++) {
                if (shell_queue.processes[i].pid == pid) {
                    sem_wait(&print_sem);
                    printf("Process with PID %d finished.\n", pid);
                    printf("  Execution Time: %lld ms (%lld time slices)\n", 
                           shell_queue.processes[i].total_execution_time,
                           shell_queue.processes[i].total_execution_time / TSLICE);
                    printf("  Completion Time: %lld ms (%lld time slices)\n", 
                           shell_queue.processes[i].completion_time,
                           shell_queue.processes[i].completion_time / TSLICE);
                    sem_post(&print_sem);
                    
                    shell_queue.processes[i].state = -1;
                    gettimeofday(&shell_queue.processes[i].end_time, NULL);
                    struct timeval elapsedTime;
                    elapsedTime.tv_sec = shell_queue.processes[i].end_time.tv_sec - shell_queue.processes[i].start_time.tv_sec;
                    elapsedTime.tv_usec = shell_queue.processes[i].end_time.tv_usec - shell_queue.processes[i].start_time.tv_usec;
                    long long elapsed = elapsedTime.tv_sec * 1000 + elapsedTime.tv_usec / 1000;
                    shell_queue.processes[i].total_execution_time += elapsed;
                }
            }
        }
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
    
    return 0;
}
