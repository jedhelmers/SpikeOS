# Userland build system for SpikeOS
CC := i686-elf-gcc
AS := i686-elf-as
CFLAGS := -nostdlib -ffreestanding -static -Wall -Wextra -O2 -g
LDFLAGS := -nostdlib -static -Wl,-Ttext=0x08048000

LIBC_SRCS := libc/string.c libc/stdio.c libc/stdlib.c libc/malloc.c libc/errno.c libc/math.c libc/pthread.c
LIBC_OBJS := $(LIBC_SRCS:.c=.o)
CRT0 := libc/crt0.o

# User programs (add more here)
PROGRAMS := hello.elf alloc_test.elf files_test.elf udp_test.elf mmap_test.elf

.PHONY: all clean

all: $(PROGRAMS)

# Build crt0
libc/crt0.o: libc/crt0.S
	$(CC) -c $< -o $@

# Build libc objects
libc/%.o: libc/%.c
	$(CC) -c $< -o $@ $(CFLAGS) -Ilibc

# Build user program objects
%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS) -Ilibc

# Link user programs: crt0 + program + libc
%.elf: %.o $(CRT0) $(LIBC_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(CRT0) $< $(LIBC_OBJS)

clean:
	rm -f $(PROGRAMS) *.o libc/*.o
