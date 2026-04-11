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
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>

#include "monitor_ioctl.h"

#define SOCKET_PATH "/tmp/container_socket"
#define STACK_SIZE (1024 * 1024)
#define BUFFER_SIZE 100
#define MAX_CONTAINERS 50

volatile int shutdown_flag = 0;

// ---------- CONTAINER ----------
struct container {
    char id[32];
    pid_t pid;
    char state[32];
    time_t start_time;

    int stop_requested;   // 🔥 TASK 4
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;

// ---------- BUFFER ----------
typedef struct {
    char data[BUFFER_SIZE][256];
    char cid[BUFFER_SIZE][32];
    int in, out, count;

    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} log_buffer_t;

log_buffer_t buffer;

// ---------- PRODUCER ----------
struct producer_args {
    int fd;
    char cid[32];
};

void *producer(void *arg) {
    struct producer_args *p = arg;
    char buf[256];

    while (1) {
        int n = read(p->fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;

        buf[n] = '\0';

        pthread_mutex_lock(&buffer.mutex);

        while (buffer.count == BUFFER_SIZE)
            pthread_cond_wait(&buffer.not_full, &buffer.mutex);

        strcpy(buffer.data[buffer.in], buf);
        strcpy(buffer.cid[buffer.in], p->cid);

        buffer.in = (buffer.in + 1) % BUFFER_SIZE;
        buffer.count++;

        pthread_cond_signal(&buffer.not_empty);
        pthread_mutex_unlock(&buffer.mutex);
    }

    close(p->fd);
    free(p);
    return NULL;
}

// ---------- CONSUMER ----------
void *consumer(void *arg) {
    while (1) {
        pthread_mutex_lock(&buffer.mutex);

        while (buffer.count == 0 && !shutdown_flag)
            pthread_cond_wait(&buffer.not_empty, &buffer.mutex);

        if (shutdown_flag && buffer.count == 0) {
            pthread_mutex_unlock(&buffer.mutex);
            break;
        }

        char line[256], cid[32];
        strcpy(line, buffer.data[buffer.out]);
        strcpy(cid, buffer.cid[buffer.out]);

        buffer.out = (buffer.out + 1) % BUFFER_SIZE;
        buffer.count--;

        pthread_cond_signal(&buffer.not_full);
        pthread_mutex_unlock(&buffer.mutex);

        char filename[64];
        sprintf(filename, "%s.log", cid);

        FILE *f = fopen(filename, "a");
        if (f) {
            fprintf(f, "%s", line);
            fclose(f);
        }
    }
    return NULL;
}

// ---------- CHILD ----------
struct child_args {
    char rootfs[128];
    int pipefd[2];
    char *cmd;
};

int child_func(void *arg) {
    struct child_args *args = arg;

    sethostname("container", 9);

    chroot(args->rootfs);
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(args->pipefd[1], STDOUT_FILENO);
    dup2(args->pipefd[1], STDERR_FILENO);

    close(args->pipefd[0]);
    close(args->pipefd[1]);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    execl("/bin/sh", "sh", "-c", args->cmd, NULL);

    perror("exec failed");
    return 1;
}

// ---------- START ----------
pid_t start_container(char *id, char *rootfs, char *cmd) {

    int pipefd[2];
    pipe(pipefd);

    struct child_args *args = malloc(sizeof(struct child_args));
    strcpy(args->rootfs, rootfs);
    args->pipefd[0] = pipefd[0];
    args->pipefd[1] = pipefd[1];
    args->cmd = strdup(cmd);

    pid_t pid = clone(child_func,
                      malloc(STACK_SIZE) + STACK_SIZE,
                      CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWPID | SIGCHLD,
                      args);

    close(pipefd[1]);

    // -------- REGISTER WITH KERNEL --------
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req;

        req.pid = pid;
        strncpy(req.container_id, id, MONITOR_NAME_LEN);

        req.soft_limit_bytes = 50 * 1024 * 1024;
        req.hard_limit_bytes = 100 * 1024 * 1024;

        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    struct producer_args *p = malloc(sizeof(struct producer_args));
    p->fd = pipefd[0];
    strcpy(p->cid, id);

    pthread_t t;
    pthread_create(&t, NULL, producer, p);
    pthread_detach(t);

    // metadata
    strcpy(containers[container_count].id, id);
    containers[container_count].pid = pid;
    strcpy(containers[container_count].state, "running");
    containers[container_count].start_time = time(NULL);
    containers[container_count].stop_requested = 0;

    container_count++;

    printf("[Supervisor] Started %s (PID: %d)\n", id, pid);

    return pid;
}

// ---------- UPDATE STATES ----------
void update_state(pid_t pid, int status) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].pid == pid) {

            if (containers[i].stop_requested) {
                strcpy(containers[i].state, "stopped");
            }
            else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
                strcpy(containers[i].state, "hard_limit_killed");
            }
            else {
                strcpy(containers[i].state, "exited");
            }

            // -------- UNREGISTER --------
            int fd = open("/dev/container_monitor", O_RDWR);
            if (fd >= 0) {
                struct monitor_request req;
                req.pid = pid;
                ioctl(fd, MONITOR_UNREGISTER, &req);
                close(fd);
            }
        }
    }
}

// ---------- STOP ----------
void stop_container(char *id) {
    for (int i = 0; i < container_count; i++) {
        if (strcmp(containers[i].id, id) == 0) {

            containers[i].stop_requested = 1;

            kill(containers[i].pid, SIGKILL);
        }
    }
}

// ---------- PS ----------
void list_containers(int fd) {
    char out[1024] = "ID\tPID\tSTATE\tSTART\n";

    for (int i = 0; i < container_count; i++) {
        char line[128];
        sprintf(line, "%s\t%d\t%s\t%ld\n",
                containers[i].id,
                containers[i].pid,
                containers[i].state,
                containers[i].start_time);
        strcat(out, line);
    }

    write(fd, out, strlen(out));
}

// ---------- SIGNAL ----------
void handle_shutdown(int sig) {
    shutdown_flag = 1;

    pthread_mutex_lock(&buffer.mutex);
    pthread_cond_broadcast(&buffer.not_empty);
    pthread_mutex_unlock(&buffer.mutex);

    printf("\n[Supervisor] Shutting down cleanly...\n");
}

// ---------- SUPERVISOR ----------
void run_supervisor() {

    signal(SIGINT, handle_shutdown);

    buffer.in = buffer.out = buffer.count = 0;
    pthread_mutex_init(&buffer.mutex, NULL);
    pthread_cond_init(&buffer.not_full, NULL);
    pthread_cond_init(&buffer.not_empty, NULL);

    pthread_t cons;
    pthread_create(&cons, NULL, consumer, NULL);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    unlink(SOCKET_PATH);
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("[Supervisor] Running...\n");

    while (!shutdown_flag) {

        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            update_state(pid, status);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);

        struct timeval tv = {1, 0};

        if (select(server_fd + 1, &fds, NULL, NULL, &tv) <= 0)
            continue;

        int client_fd = accept(server_fd, NULL, NULL);

        char buf[256] = {0};
        read(client_fd, buf, sizeof(buf));

        if (strncmp(buf, "start", 5) == 0) {
            char id[32], rootfs[128], cmd[256];
            sscanf(buf, "start %s %s %[^\n]", id, rootfs, cmd);
            start_container(id, rootfs, cmd);
            write(client_fd, "OK\n", 3);
        }

        else if (strncmp(buf, "run", 3) == 0) {
            char id[32], rootfs[128], cmd[256];
            sscanf(buf, "run %s %s %[^\n]", id, rootfs, cmd);

            pid_t pid = start_container(id, rootfs, cmd);

            int status;
            waitpid(pid, &status, 0);
            update_state(pid, status);

            write(client_fd, "DONE\n", 5);
        }

        else if (strncmp(buf, "ps", 2) == 0) {
            list_containers(client_fd);
        }

        else if (strncmp(buf, "stop", 4) == 0) {
            char id[32];
            sscanf(buf, "stop %s", id);
            stop_container(id);
            write(client_fd, "STOPPED\n", 8);
        }

        else if (strncmp(buf, "logs", 4) == 0) {
            write(client_fd, "Check <id>.log file\n", 21);
        }

        close(client_fd);
    }

    pthread_join(cons, NULL);
}

// ---------- CLIENT ----------
void run_client(int argc, char *argv[]) {

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    char buf[256] = {0};

    for (int i = 1; i < argc; i++) {
        strcat(buf, argv[i]);
        strcat(buf, " ");
    }

    write(sock, buf, strlen(buf));

    char response[1024] = {0};
    read(sock, response, sizeof(response));
    printf("%s", response);

    close(sock);
}

// ---------- MAIN ----------
int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
        run_supervisor();
    else
        run_client(argc, argv);

    return 0;
}
