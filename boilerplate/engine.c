#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define SOCKET_PATH "/tmp/container_socket"
#define MAX_CONTAINERS 10

struct child_args {
    char *rootfs;
    int pipefd[2];
};

struct container {
    char id[32];
    pid_t pid;
    int running;
    int pipefd;
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;

#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];


// 🔥 NEW: REGISTER FUNCTION
void register_pid_with_kernel(pid_t pid, const char *id) {

    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("open device");
        return;
    }

    struct monitor_request req;

    req.pid = pid;
    req.soft_limit_bytes = 50 * 1024 * 1024;
    req.hard_limit_bytes = 100 * 1024 * 1024;
    strncpy(req.container_id, id, MONITOR_NAME_LEN);

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl register");
    } else {
        printf("[ENGINE] Registered PID %d (%s)\n", pid, id);
    }

    close(fd);
}


// ---------------- CHILD ----------------
int child_func(void *arg) {
    struct child_args *args = (struct child_args *)arg;
    char *rootfs = args->rootfs;
    int *pipefd = args->pipefd;

    close(pipefd[0]);

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    close(pipefd[1]);

    printf("[Child] Starting container...\n");

    sethostname("container", 9);

    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        return 1;
    }

    printf("[Child] Inside container!\n");

    execl("/bin/memory_hog","/bin/memory_hog","4","200",NULL);
    perror("execl");
    return 1;
}


// ---------------- START ----------------
pid_t start_container(char *id, char *rootfs) {

    if (container_count >= MAX_CONTAINERS) {
        printf("Max containers reached\n");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    struct child_args *args = malloc(sizeof(struct child_args));
    args->rootfs = rootfs;
    args->pipefd[0] = pipefd[0];
    args->pipefd[1] = pipefd[1];

    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_func, child_stack + STACK_SIZE, flags, args);

    if (pid < 0) {
        perror("clone");
        return -1;
    }

    close(pipefd[1]);

    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    containers[container_count].running = 1;
    containers[container_count].pipefd = pipefd[0];

    container_count++;

    printf("[Parent] Started container %s with PID %d\n", id, pid);

    // 🔥 NEW: REGISTER WITH KERNEL
    register_pid_with_kernel(pid, id);

    return pid;
}


// ---------------- LIST ----------------
void list_containers() {
    printf("ID\tPID\tSTATE\n");

    for (int i = 0; i < container_count; i++) {
        printf("%s\t%d\t%s\n",
               containers[i].id,
               containers[i].pid,
               containers[i].running ? "running" : "stopped");
    }
}


// ---------------- MAIN ----------------
int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        printf("  engine supervisor <rootfs>\n");
        printf("  engine start <id> <rootfs> <cmd>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            perror("socket");
            return 1;
        }

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        unlink(SOCKET_PATH);

        if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind");
            return 1;
        }

        if (listen(server_fd, 5) < 0) {
            perror("listen");
            return 1;
        }

        printf("[Supervisor] Running...\n");

        while (1) {

            while (waitpid(-1, NULL, WNOHANG) > 0);

            fd_set readfds;
            FD_ZERO(&readfds);

            FD_SET(server_fd, &readfds);
            int maxfd = server_fd;

            for (int i = 0; i < container_count; i++) {
                if (containers[i].running) {
                    FD_SET(containers[i].pipefd, &readfds);
                    if (containers[i].pipefd > maxfd)
                        maxfd = containers[i].pipefd;
                }
            }

            int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);

            if (activity < 0) {
                perror("select");
                continue;
            }

            if (FD_ISSET(server_fd, &readfds)) {

                int client_fd = accept(server_fd, NULL, NULL);
                if (client_fd < 0) {
                    perror("accept");
                    continue;
                }

                char buffer[256] = {0};
                int n = read(client_fd, buffer, sizeof(buffer) - 1);

                if (n > 0) {
                    buffer[n] = '\0';

                    printf("[Supervisor] Received: %s\n", buffer);

                    if (strncmp(buffer, "start", 5) == 0) {
                        char id[32], rootfs[128], cmd[128];
                        sscanf(buffer, "start %s %s %s", id, rootfs, cmd);

                        printf("[Supervisor] Starting container %s...\n", id);
                        start_container(id, rootfs);
                    }

                    else if (strncmp(buffer, "ps", 2) == 0) {
                        list_containers();
                    }
                }

                close(client_fd);
            }

            for (int i = 0; i < container_count; i++) {

                if (containers[i].running &&
                    FD_ISSET(containers[i].pipefd, &readfds)) {

                    char buffer[256];
                    int n = read(containers[i].pipefd,
                                 buffer,
                                 sizeof(buffer) - 1);

                    if (n <= 0) {
                        close(containers[i].pipefd);
                        containers[i].running = 0;
                        continue;
                    }

                    buffer[n] = '\0';

                    char *line = strtok(buffer, "\n");
                    while (line != NULL) {
                        printf("[LOG %s]: %s\n",
                               containers[i].id,
                               line);
                        line = strtok(NULL, "\n");
                    }

                    fflush(stdout);
                }
            }
        }
    }

    else if (strcmp(argv[1], "start") == 0) {

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            return 1;
        }

        char buffer[256];
        snprintf(buffer, sizeof(buffer),
                 "start %s %s %s",
                 argv[2], argv[3], argv[4]);

        write(sock, buffer, strlen(buffer));
        close(sock);
    }

    else if (strcmp(argv[1], "ps") == 0) {

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("connect");
            return 1;
        }

        write(sock, "ps", 2);
        close(sock);
    }

    return 0;
}
