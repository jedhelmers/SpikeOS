# SpikeOS
An x86 learning experiment in loving memory of my boy. 

## Arch
For this project I'm working through the wonderful work outlined via [wiki.osdev.org](wiki.osdev.org), PDFs from the [University of Winsconsin-Madison](https://pages.cs.wisc.edu/~remzi/OSTEP/#book-chapters), and the classic book, *OPERATING SYSTEMS - Design & Implementation* by Andrew S. Tanenbaum. **OsDev.org** largely focusses on x86 and the others are just incredibly good resources.

Since I'm working on Apple Silicon, I have to use these
```
brew install \
  i386-elf-gcc \
  nasm \
  qemu \
  grub \
  xorriso \
  mtools
```

What each is for:
    * i386-elf-gcc - Cross-compiler for building your kernel
    * nasm - Assembler (x86 assembly)
    * qemu - Emulator to run your OS
    * grub - Bootloader
    * xorriso - ISO creation tool
    * mtools - FAT filesystem tools (used in some boot/ISO workflows)

## Get it going
Commands:
```
./clean.sh
./headers.sh
./qemu.sh
```

Simply run `./qemu.sh` to build it and run it.

## Complete
As of now, we have a bootloader, a cross-compiler, GDT, and IDT as well as a VGA display. The C code is freestanding so.
