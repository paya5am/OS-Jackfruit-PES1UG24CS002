#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <string.h>
#define MAX_CONTAINERS 10

struct container {
    char id[32];
    pid_t pid;
    int running;
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;
#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];

int child_func(void *arg) {
    char *rootfs = (char *)arg;

    printf("[Child] Starting container...\n");

    // Set hostname (UTS namespace)
    sethostname("container", 9);

    // Change root filesystem
    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    // Mount /proc inside container
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        return 1;
    }

    printf("[Child] Inside container!\n");

    // Run shell
    execl("/bin/sh", "/bin/sh", NULL);

    perror("execl");
    return 1;
}
pid_t start_container(char *id, char *rootfs) {
    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_func, child_stack + STACK_SIZE, flags, rootfs);

    if (pid < 0) {
        perror("clone");
        return -1;
    }

    // store metadata
    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    containers[container_count].running = 1;

    container_count++;

    printf("[Parent] Started container %s with PID %d\n", id, pid);

    return pid;
}
int main() {
    printf("[Parent] Starting multiple containers...\n");

    start_container("alpha", "./rootfs-alpha");
    start_container("beta", "./rootfs-beta");

    // print metadata
    printf("\nContainer List:\n");
    printf("ID\tPID\tSTATE\n");

    for (int i = 0; i < container_count; i++) {
        printf("%s\t%d\t%s\n",
            containers[i].id,
            containers[i].pid,
            containers[i].running ? "running" : "stopped");
    }

    // wait for containers
    for (int i = 0; i < container_count; i++) {
        waitpid(containers[i].pid, NULL, 0);
        containers[i].running = 0;
    }

    return 0;
}
