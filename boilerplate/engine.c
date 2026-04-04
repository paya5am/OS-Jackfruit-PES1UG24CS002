#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <string.h>

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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <rootfs>\n", argv[0]);
        return 1;
    }

    char *rootfs = argv[1];

    printf("[Parent] Launching container...\n");

    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_func, child_stack + STACK_SIZE, flags, rootfs);

    if (pid < 0) {
        perror("clone");
        return 1;
    }

    printf("[Parent] Container PID: %d\n", pid);

    waitpid(pid, NULL, 0);

    printf("[Parent] Container exited.\n");

    return 0;
}
