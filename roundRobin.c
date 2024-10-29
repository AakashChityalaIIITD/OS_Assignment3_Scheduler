#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#define MAXPWD 1000
#define MAXHISTORY 1000

typedef struct {
    char name[10];
    int pid;
    double burst;
    double remaining_time;
    double waiting_time;
    double completion_time;
    int priority;
} Process;

typedef struct {
    Process items[MAXPWD];
    int front;
    int rear;
} Queue;

void initializeQueue(Queue* queue) {
    queue->front = 0;
    queue->rear = 0;    
}

int isEmpty(Queue* queue) {
    return queue->front == queue->rear;
}

void enqueue(Queue* queue, Process process) {
    if (queue->rear < MAXPWD) {
        queue->items[queue->rear] = process;
        queue->rear++;
    } else {
        printf("Queue is full\n");
    }
}

Process dequeue(Queue* queue) {
    if (isEmpty(queue)) {
        perror("Queue is empty\n");
        Process empty_process = { "", 0, 0, 0, 0, 0, 0 };
        return empty_process;
    } else {
        Process item = queue->items[queue->front];
        queue->front++;
        return item;
    }
}

Process view(Queue* queue) {
    if (isEmpty(queue)) {
        perror("Queue is empty\n");
        Process empty_process = { "", 0, 0, 0, 0, 0, 0 };
        return empty_process;
    } else {
        return queue->items[queue->front];
    }
}

void roundRobinAlgo(Queue* ready_queue, Process processes[], int NCPU, int no_of_processes, double tSlice) {
    double current_time = 0;
    int completed_processes = 0;

    Process process_on_cpu[NCPU];
    int processor_state[NCPU];

    for (int i = 0; i < NCPU; i++) {
        processor_state[i] = -1;
    }

    while (completed_processes < no_of_processes) {
        
        // Assign processes to each available CPU
        for (int i = 0; i < NCPU; i++) {
            if (!isEmpty(ready_queue) && processor_state[i] == -1) {
                process_on_cpu[i] = dequeue(ready_queue);
                processor_state[i] = 1; // CPU i is active
            }
        }

        // Process each CPU's task simultaneously
        for (int i = 0; i < NCPU; i++) {
            if (processor_state[i] == 1) {
                double time_on_cpu;
                
                if (process_on_cpu[i].remaining_time < tSlice) {
                    time_on_cpu = process_on_cpu[i].remaining_time;
                } else {
                    time_on_cpu = tSlice;
                }

                process_on_cpu[i].remaining_time -= time_on_cpu;

                printf("Time %lf - %lf: Running PID:%d on CPU %d\n", current_time, current_time + time_on_cpu, process_on_cpu[i].pid, i + 1);

                // Check if process is complete
                if (process_on_cpu[i].remaining_time == 0) {
                    for (int j = 0; j < no_of_processes; j++) {
                        if (processes[j].pid == process_on_cpu[i].pid) {
                            processes[j].completion_time = current_time + time_on_cpu;
                            break;
                        }
                    }
                    completed_processes++;
                    processor_state[i] = -1; // Free the CPU
                } else {
                    // Requeue the process if it is not finished
                    enqueue(ready_queue, process_on_cpu[i]);
                    processor_state[i] = -1; // Free the CPU for next cycle
                }
            }
        }
        
        // increase current time only when all CPU(s) have completed their timeslice
        current_time += tSlice;
    }

    // Calculate waiting time for each process
    for (int i = 0; i < no_of_processes; i++) {
        processes[i].waiting_time = processes[i].completion_time - processes[i].burst;
    }

    // Print process summary
    printf("%-5s %-40s %-6s %-15s %-15s %s\n", "No.", "Process Name", "PID", "Burst time", "Completion Time", "Wait Time");

    for (int i = 0; i < no_of_processes; i++) {
        printf("%-5d %-40s %-6d %-15.2f %-15.2f %.2f\n", i + 1, processes[i].name, processes[i].pid, processes[i].burst, processes[i].completion_time, processes[i].waiting_time);
    }

    double total_waiting_time = 0;
    double total_completion_time = 0;
    for (int i = 0; i < no_of_processes; i++) {
        total_waiting_time += processes[i].waiting_time;
        total_completion_time += processes[i].completion_time;
    }

    printf("The average waiting time of round-robin scheduling process is: %lf\n", total_waiting_time / no_of_processes);
    printf("The average completion time of round-robin scheduling process is: %lf\n", total_completion_time / no_of_processes);
}

int main() {
    Queue ready_queue;
    initializeQueue(&ready_queue);

    Process processes[] = {
        { "P1", 1, 10, 10, 0, 0, 3 },
        { "P2", 2, 15, 15, 0, 0, 1 },
        { "P3", 3, 20, 20, 0, 0, 2 }
    };

    enqueue(&ready_queue, processes[0]);
    enqueue(&ready_queue, processes[1]);
    enqueue(&ready_queue, processes[2]);

    int no_of_processes = 3;
    int NCPU = 3;
    double tSlice = 5;

    roundRobinAlgo(&ready_queue, processes, NCPU, no_of_processes, tSlice);

    return 0;
}
