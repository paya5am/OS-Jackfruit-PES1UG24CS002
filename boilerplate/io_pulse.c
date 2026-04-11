#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DEFAULT_OUTPUT "/tmp/io_pulse.out"

int main(int argc, char *argv[])
{
    unsigned int sleep_ms = 200;

    if (argc > 2)
        sleep_ms = atoi(argv[2]);

    int fd = open(DEFAULT_OUTPUT, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    unsigned int i = 0;

    while (1) {
        i++;

        char buf[64];
        int len = sprintf(buf, "io_pulse iteration=%u\n", i);

        write(fd, buf, len);
        fsync(fd);

        printf("io_pulse wrote iteration=%u\n", i);
        fflush(stdout);

        usleep(sleep_ms * 1000);
    }

    close(fd);
    return 0;
}
