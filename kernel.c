#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Check if the compilter thinks you are targeting the wrong OS.
#if defined(__linux__)
#error "You are not using a cross-compilter, you will most certainly run into trouble"
#endif

// This tutorial will only work for the 32-bit ix86 targets.
#if !defined(__i686__) && !defined(__i386__)
#error "This tutorial needs to be compiled with an ix86-elf compiler"
#endif


// Hardware text mode color constants.
enum vga_color {
	VGA_COLOR_BLACK = 0,
	VGA_COLOR_BLUE = 1,
	VGA_COLOR_GREEN = 2,
	VGA_COLOR_CYAN = 3,
	VGA_COLOR_RED = 4,
	VGA_COLOR_MAGENTA = 5,
	VGA_COLOR_BROWN = 6,
	VGA_COLOR_LIGHT_GREY = 7,
	VGA_COLOR_DARK_GREY = 8,
	VGA_COLOR_LIGHT_BLUE = 9,
	VGA_COLOR_LIGHT_GREEN = 10,
	VGA_COLOR_LIGHT_CYAN = 11,
	VGA_COLOR_LIGHT_RED = 12,
	VGA_COLOR_LIGHT_MAGENTA = 13,
	VGA_COLOR_LIGHT_BROWN = 14,
	VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t) uc | (uint16_t) color << 8;
}

size_t strlen(const char* str) {
    size_t len = 0;

    while(str[len]) {
        len++;
    }

    return len;
}

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

size_t      terminal_row;
size_t      terminal_column;
uint8_t     terminal_color;
uint16_t*   terminal_buffer = (uint16_t*)VGA_MEMORY;

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_color = vga_entry_color(((y + x) % 15) + 1, VGA_COLOR_BLACK);
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry('-', terminal_color);
        }
    }
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_scroll(void) {
    // Shift the terminal buffer index
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];
        }
    }

    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry('-', terminal_color);
    }
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_putchar(char c) {
    // Linebreak
    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
    } else {
        terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
        terminal_column++;
    }

    // Wrap text
    if (terminal_column >= VGA_WIDTH) {
        terminal_column = 0;
        terminal_row++;
    }

    // Scroll
    if (terminal_row >= VGA_HEIGHT) {
        terminal_scroll();
        terminal_row = VGA_HEIGHT - 1;
    }
}

void terminal_write(const char* data, size_t size) {
    // Iterate through each char in the data
    for (size_t i = 0; i < size; i++) {
        terminal_putchar(data[i]);
    }
}

void terminal_writestring(const char* data) {
    terminal_write(data, strlen(data));
}

void kernel_main(void) {
    // initialize terminal interface
    terminal_initialize();

    // Newline support is left as an exercise
    terminal_writestring("Lorem ipsum dolor sit amet, consectetur adipiscing elit. Maecenas magna libero, lobortis a mattis at, elementum eu augue. Praesent non sagittis purus. Sed laoreet mi sed magna interdum, sed suscipit mi malesuada. Cras vel nisi velit. Ut malesuada semper tellus, vitae posuere nisi fermentum eu. Cras rutrum sapien nisi, in convallis augue dictum a. Mauris nibh est, tincidunt eget quam sit amet, convallis lobortis nisi. Mauris mattis justo mi, id scelerisque ante ullamcorper ut. Maecenas aliquam facilisis consectetur. Praesent enim nisl, ullamcorper in lacus et, aliquet accumsan erat. Nulla eget nibh vitae dui porta convallis ac nec libero. Praesent malesuada dui vitae justo dictum, ac semper dui accumsan. Donec nec orci est. Sed gravida vel risus in efficitur.\n\nPellentesque viverra tellus id semper auctor. Proin sit amet odio id elit posuere vehicula sit amet quis nunc. Phasellus sit amet pellentesque orci. Vestibulum eu augue maximus, porta enim et, scelerisque risus. Curabitur sit amet dui est. Morbi nec dignissim tellus, ac varius justo. Phasellus sollicitudin lectus sem, in posuere metus viverra posuere. Curabitur laoreet enim quam. Pellentesque sodales elit urna, nec ultrices nibh mollis ut.\n\nVivamus urna nibh, rutrum eu pharetra et, faucibus congue ipsum. Donec faucibus nibh et risus placerat, id iaculis mi dapibus. Etiam non massa viverra, rhoncus odio eget, consequat metus. Nullam pretium vel justo rutrum pellentesque. In egestas aliquet nulla, et fermentum ex venenatis ac. Pellentesque sagittis ultrices eros et fermentum. Mauris erat mauris, sodales ac mi sed, ullamcorper mattis nisl. Proin posuere interdum elementum. Vestibulum vitae magna ac nisi viverra accumsan. In dapibus ante sed nibh tincidunt, et maximus elit aliquam. Morbi ullamcorper dui nec dui lacinia semper luctus sit amet urna. Curabitur nibh nulla, molestie eu nunc eget, congue ultrices risus.");
    // terminal_writestring("Howdy");
}