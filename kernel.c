#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct IDTR {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));
struct IDTR idtr;

struct IDT {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t type_attr;
    uint16_t offset_high;
} __attribute__((packed));
struct IDT idt[256];

/* Check if the compiler thinks you are targeting the wrong operating system. */
#if defined(__linux__)
#error "You are not using a cross-compiler, you will most certainly run into trouble"
#endif

/* This tutorial will only work for the 32-bit ix86 targets. */
#if !defined(__i386__)
#error "This tutorial needs to be compiled with a ix86-elf compiler"
#endif

struct GDT {
    uint32_t base;
    uint32_t limit;
    uint8_t access_byte;
    uint8_t flags;
};

void kerror(const char* msg)
{
    (void)msg;
    for(;;) __asm__("hlt");  // halt if dies
}

void encodeGdtEntry(uint8_t *target, struct GDT source)
{
    // check if it can be encoded based on limit
    if (source.limit > 0xFFFFF) {kerror("GDT cannot encode limits larger than 0xFFFFF");}

    // encode limit
    target[0] = source.limit & 0xFF;
    target[1] = (source.limit >> 8) & 0xFF;
    target[6] = (source.limit >> 16) & 0x0F;

    // base
    target[2] = source.base & 0xFF;
    target[3] = (source.base >> 8) & 0xFF;
    target[4] = (source.base >> 16) & 0xFF;
    target[7] = (source.base >> 24) & 0xFF;

    // access byte
    target[5] = source.access_byte;

    // flags
    target[6] |= (source.flags << 4);
}

/* Hardware text mode color constants. */
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

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color)
{
    return (uint16_t) uc | (uint16_t) color << 8;
}

void set_idt_entry(int num, uint32_t handler) {
    idt[num].offset_low = handler & 0xFFFF;
    idt[num].selector = 0x08;
    idt[num].zero = 0;
    idt[num].type_attr = 0x8E;
    idt[num].offset_high = (handler >> 16) & 0xFFFF;
}

size_t strlen(const char* str)
{
    size_t len = 0;
    while (str[len])
        len++;
    return len;
}

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;

#define max_output 1024
volatile size_t output_index = 0;
volatile char out_buffer[max_output];

void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);

    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}

void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

void default_handler() {
    outb(0x20, 0x20);
    outb(0xA0, 0x20);
}

uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void pic_remap() {
    //intialize
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // set offsets
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    //set mode
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    //unmask
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void terminal_setcolor(uint8_t color)
{
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y)
{
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_scrolldown() {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = terminal_buffer[(y + 1) * VGA_WIDTH +x];
        }
    }
    for(size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
    terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c)
{
    //new line feature
    if(c == '\n') {                         // if \n detected in text
        terminal_column = 0;                // go to start of line (char 0)
        if (++terminal_row == VGA_HEIGHT)   // go below one line
            terminal_scrolldown();               // scroll down
        return;
    }

    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scrolldown();
    }
}

void terminal_write(const char* data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data)
{
    terminal_write(data, strlen(data));
}

//compare buffer to list of commands to see if its available
int strcmp_buf(const char* cmd) {
    size_t len = strlen(cmd);
    if (output_index != len) return 0;  // if lengths differ, there wont be a match
    for (size_t i = 0; i < len; i++) {
        if (out_buffer[i] != cmd[i]) return 0;  // if any char differs no match
    }
    return 1;  // something matches
}

void print_buffer(void) {
    // LIST OF COMMANDS //
    // ---------------------------------
    // hello - writes "hello world!"
    // ---------------------------------
    // clr - clear screen
    // ---------------------------------
    // shutdown - shutdown the machine
    // ---------------------------------
    // reboot - reset the machine
    // ---------------------------------

    //simple hello comand
    if(strcmp_buf("hello")) {
        terminal_writestring("\nhello world!!\n");
    }


    //clear by basically reinitilizing the terminal
    else if(strcmp_buf("clr")) {
        terminal_initialize();
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    }

    else if(strcmp_buf("shutdown")) {
        terminal_writestring("\nshutting down... goodbye!\n");
        outw(0x604, 0x2000); //QEMU method
    }


    //rebooting (not tested on real hardware but works under QEMU) https://wiki.osdev.org/Reboot
    else if(strcmp_buf("reboot")) {
        terminal_writestring("\nrebooting... goodbye!\n");
        uint8_t g = 0x02;
        while(g & 0x02)
            g = inb(0x64);
        outb(0x64, 0xFE);
    }

    //handle unknown commands
    else {
        terminal_writestring("\nunknown command: ");
        for (size_t i = 0; i < output_index; i++) {
            terminal_putchar(out_buffer[i]);
        }
        terminal_writestring("\n");
    }
}

void keyboard_handler() {
    uint8_t scancode = inb(0x60);
    if (scancode & 0x80) {
        outb(0x20, 0x20);  // still need to send EOI on key release
        return;
    }
    char c = '>';

    //                           //
    //  ALWAYS SET 1 NO DEMENTIA //
    //                           //

    //literally all the letters by scan code according to set 1 in alphabetical order(PS2 only)
    if (scancode == 0x1E) c = 'a';
    if (scancode == 0x30) c = 'b';
    if (scancode == 0x2E) c = 'c';
    if (scancode == 0x20) c = 'd';
    if (scancode == 0x12) c = 'e';
    if (scancode == 0x21) c = 'f';
    if (scancode == 0x22) c = 'g';
    if (scancode == 0x23) c = 'h';
    if (scancode == 0x17) c = 'i';
    if (scancode == 0x24) c = 'j';
    if (scancode == 0x25) c = 'k';
    if (scancode == 0x26) c = 'l';
    if (scancode == 0x32) c = 'm';
    if (scancode == 0x31) c = 'n';
    if (scancode == 0x18) c = 'o';
    if (scancode == 0x19) c = 'p';
    if (scancode == 0x10) c = 'q';
    if (scancode == 0x13) c = 'r';
    if (scancode == 0x1F) c = 's';
    if (scancode == 0x14) c = 't';
    if (scancode == 0x16) c = 'u';
    if (scancode == 0x2F) c = 'v';
    if (scancode == 0x11) c = 'w';
    if (scancode == 0x2D) c = 'x';
    if (scancode == 0x15) c = 'y';
    if (scancode == 0x2C) c = 'z';


    //numbers
    if (scancode == 0x0B) c = '0';
    if (scancode == 0x02) c = '1';
    if (scancode == 0x03) c = '2';
    if (scancode == 0x04) c = '3';
    if (scancode == 0x05) c = '4';
    if (scancode == 0x06) c = '5';
    if (scancode == 0x07) c = '6';
    if (scancode == 0x08) c = '7';
    if (scancode == 0x09) c = '8';
    if (scancode == 0x0A) c = '9';



    //other characters
    /*  tab  */
    if(scancode == 0x0F) terminal_writestring("        ");
    /* space */
    if (scancode == 0x39) c = ' '; 
    /* the slash */
    if (scancode == 0x35) c = '/';
    /* enter */
    if (scancode == 0x1C) {
    if (output_index > 0) {
        print_buffer();
        output_index = 0;
        terminal_writestring("geese->");
    }
    outb(0x20, 0x20);
    return;
    }
    /* backspace*/ if(scancode == 0x0E) {
        if(terminal_column > 0) {
            terminal_column--;
        }
        else if(terminal_row > 0) {
            terminal_row--;
            terminal_column = VGA_WIDTH - 1;
        }
        terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);

        //remove last char from buffer too
        if(output_index > 0) output_index--;
        outb(0x20, 0x20);
        return;
    }
    
    if(c != '>') {//ignore if invalid/unconfiged scancode{
                 //add character to buffer
        if(output_index < max_output) {
            out_buffer[output_index++] = c;
        }

        terminal_putchar(c);
    }

    outb(0x20, 0x20);
}

extern void keyboard_isr(void);

void kernel_main(void)
{
    terminal_initialize();
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("hello!\nthis is a new line..\n");
    terminal_writestring("its alive!!\n");
    terminal_writestring("@>");
    pic_remap();
    extern void default_isr(void);
    for (int i = 0; i < 256; i++)
        set_idt_entry(i, (uint32_t)default_isr);

    set_idt_entry(33, (uint32_t)keyboard_isr);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint32_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    __asm__ volatile ("sti");
    for(;;) __asm__("hlt");
}