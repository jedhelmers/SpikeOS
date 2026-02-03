#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s /dev/ttysXXX\n", argv[0]);
        return 1;
    }

    // Create file descriptor using the pseudo-terminal
    // This acts like physcal wires between systems
    int fd = open(argv[1], O_RDWR | O_NOCTTY);

    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("Connected to %s\n", argv[1]);

    while (1) {
        // Write panic bit for now.
        const char msg[] = {0XFF};
        write(fd, msg, sizeof(msg));
        printf("Sent panic byte\n");
        sleep(1);
    }
}