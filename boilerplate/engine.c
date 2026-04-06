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
#define SOCKET_PATH "/tmp/container_socket"
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

    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    containers[container_count].running = 1;

    container_count++;

    printf("[Parent] Started container %s with PID %d\n", id, pid);

    return pid;
}

void list_containers() {
    printf("ID\tPID\tSTATE\n");

    for (int i = 0; i < container_count; i++) {
        printf("%s\t%d\t%s\n",
            containers[i].id,
            containers[i].pid,
            containers[i].running ? "running" : "stopped");
    }
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        printf("  engine supervisor <rootfs>\n");
        printf("  engine start <id> <rootfs> <cmd>\n");
        return 1;
    }

    // ---------------- SUPERVISOR ----------------
    if (strcmp(argv[1], "supervisor") == 0) {

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            perror("socket");
            return 1;
        }

        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        unlink(SOCKET_PATH);

        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind");
            return 1;
        }

        if (listen(server_fd, 5) < 0) {
            perror("listen");
            return 1;
        }

        printf("[Supervisor] Running...\n");

        while (1) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd < 0) {
                perror("accept");
                continue;
            }

            char buffer[256] = {0};
            read(client_fd, buffer, sizeof(buffer));

            printf("[Supervisor] Received: %s\n", buffer);

            // -------- START COMMAND --------
            if (strncmp(buffer, "start", 5) == 0) {

                char id[32], rootfs[128], cmd[128];

                sscanf(buffer, "start %s %s %s", id, rootfs, cmd);

                printf("[Supervisor] Starting container %s...\n", id);

                start_container(id, rootfs);
            }

            // -------- PS COMMAND --------
            else if (strncmp(buffer, "ps", 2) == 0) {
                printf("[Supervisor] Listing containers:\n");
                list_containers();
            }

            close(client_fd);
        }
    }

    // ---------------- CLIENT ----------------
    else if (strcmp(argv[1], "start") == 0) {

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            return 1;
        }

        char buffer[256];
        snprintf(buffer, sizeof(buffer), "start %s %s %s",
                 argv[2], argv[3], argv[4]);

        write(sock, buffer, strlen(buffer));

        close(sock);
    }

    // -------- PS CLIENT --------
    else if (strcmp(argv[1], "ps") == 0) {

        int sock = socket(AF_UNIX, SOCK_STREAM, 0);

        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, SOCKET_PATH);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("connect");
            return 1;
        }

        write(sock, "ps", 2);

        close(sock);
    }

    return 0;
}
