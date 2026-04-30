#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//font
#include "font8x8_basic.h"


//programs
#include "badapple.h"

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

struct multiboot {
    uint32_t flags;
    uint32_t memlow;
    uint32_t memup;
    uint32_t boot_dev;
    uint32_t cmd;
    uint32_t mods_loaded;
    uint32_t mods_address;
    uint8_t pads[60];
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
} __attribute__((packed));

struct multimod {
    uint32_t start;
    uint32_t end;
    uint32_t cmd;
    uint32_t reserved;
};

#define max_files 8
#define max_name 16
#define max_size (1024 * 1024 * 8)

struct file {
    char name[max_name];
    char data[max_size];
    size_t size;
    int used; //1 if there is a file
};

struct file* ramdisk;

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

//framebuffer
uint32_t* fb;
uint32_t fb_pitch;
uint32_t fb_width;
uint32_t fb_height;
uint32_t fb_bpp; 

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y);
void terminal_scrolldown(void);
void initpit(int hz);
extern void halt(void);

//bump and heap
#define heapsize (1024 * 1024)
static uint8_t heap[heapsize];
static size_t heaptop = 0;

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

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t first_table[1024] __attribute__((aligned(4096)));

//framebuffer table
uint32_t fb_table[1024] __attribute__((aligned(4096)));

//blank page directory
void blank_pagedir() {
    int i;
    for(i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002;
    }
}

void create_table(uint32_t fb_phys) {
    unsigned int i;
    //4 megabyte
    for(i = 0; i < 1024; i++) {
        first_table[i] = (i * 0x1000) | 3;
    }
    //put the page into directory
    page_directory[0] = ((unsigned int)first_table) | 3;

    //framebuffer | 4MB
    fb_phys = fb_phys & 0xFFC00000;
    for(i = 0; i < 1024; i++) {
        fb_table[i] = (fb_phys + i * 0x1000) | 3;
    }
    page_directory[fb_phys >> 22] = ((uint32_t)fb_table) | 3;
}

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

extern void halt(void);

volatile uint32_t countdown;

volatile uint32_t ticks = 0;

volatile uint8_t cursor_visibility = 0;
volatile uint8_t cursor_timer = 0;

volatile uint8_t can_print_char = 0;
volatile uint8_t esc_pressed = 0;
volatile uint8_t shift_pressed = 0;


volatile uint8_t blinktime = 500;

void timer_irq() {
    ticks++;
    if(!can_print_char) {
        return;
    }
    cursor_timer++;
    //blink every 500ms or whatever value the blinktime int above is
    
    if(cursor_timer >= blinktime) {
        cursor_timer = 0;
        cursor_visibility = !cursor_visibility;
        draw_textcursor();
    }

}

void sleep(uint32_t ms) {
    uint32_t start = ticks;
    while ((ticks - start) < ms) {
        __asm__ volatile ("sti; hlt");
    }
}

//copy string manualy
void strcpy_s(char* dst, const char* src, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}
//copy memory manually
void memcpy_s(char* dst, const char* src, size_t size) {
    for (size_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

#define VGA_WIDTH   80
#define VGA_HEIGHT  60
#define VGA_MEMORY  0xB8000

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;

void placepixel(int x, int y, uint32_t color) {
    uint8_t* where = (uint8_t*)fb + y * fb_pitch + x * fb_bpp;
    where[0] = color         & 0xFF;  //red
    where[1] = (color >> 8)  & 0xFF;  //green
    where[2] = (color >> 16) & 0xFF;  //blue
}

void draw_char(char c, int x, int y, uint32_t fg, uint32_t bg) {
    uint8_t* glyph = (uint8_t*)font8x8_basic[(uint8_t)c];
    for(int row = 0; row < 8; row++) {
        for(int col = 0; col < 8; col++) {
            if(glyph[row] & (1 << col))
                placepixel(x + col, y + row, fg);
            else
                placepixel(x + col, y + row, bg);
        }
    }
}

void draw_frame(uint8_t* frame, uint8_t scale) {
                        //vertical 96
    for (int y = 0; y < 96; y++) {
        for(int x = 0; x < 128; x++)  { //horizontal 128
            int i = y * 128 + x;

            uint8_t byte = frame[i / 8];
            uint8_t bit = (byte >> (7 - (i % 8))) & 1;

            uint32_t color = bit ? 0xFFFFFF : 0x000000;

            int sx = 1;
            int sy = 1;

            if(scale == 1) {
                sx = x * 5;
                sy = y * 5;
            }

            for (int dy = 0; dy < 5; dy++) {
                for (int dx = 0; dx < 5; dx++) {
                    placepixel(sx + dx, sy + dy, color);
                }
            }
    }
}
}

void clear_pixels() {
    for (uint32_t y = 0; y < fb_height; y++) {
        for (uint32_t x = 0; x < fb_width; x++) {
            placepixel(x, y, 0x00000000);
        }
    }
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

void* malloc(size_t size) {
    //align the size to 8 bytes
    size = (size + 7) & ~7;

    if (heaptop + size >= heapsize) {
        terminal_writestring("\nran out of memory~\n");
        return 0;
    }

    void* ptr = heap[heaptop];
    heaptop += size;
    return ptr;
}

void initramdisk(struct multiboot* m) {
//make everything blank (0)
    for (int i = 0; i < max_files; i++) {
        ramdisk[i].used = 0;
    }
    //if no module do nothing
    if(m->mods_loaded == 0) { terminal_writestring("no modules loaded"); terminal_writestring("\n"); return; }
    struct multimod* mod = (struct multimod*)m->mods_address;

    for(uint32_t i = 0; i < m->mods_loaded && i < max_files; i++) {

        size_t size = mod[i].end - mod[i].start;

        //skip files larger than max_size
        if(size > max_size) {
            terminal_writestring("FILE SKIPPED: size too big");
            continue;
        }

        //copy file name via strcpy_s
        strcpy_s(ramdisk[i].name, (const char*)mod[i].cmd, max_name);
        //copy from ram to ramdisk slot
        memcpy_s(ramdisk[i].data, (const char*)mod[i].start, size);

        ramdisk[i].size = size;
        ramdisk[i].used = 1;

    }
}

#define max_output 1024
volatile size_t output_index = 0;
volatile char out_buffer[max_output];

void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = 0x0F;

    for (uint32_t y = 0; y < fb_height; y++) {
        uint8_t* row = (uint8_t*)fb + y * fb_pitch;
        for(uint32_t x = 0; x < fb_width * fb_bpp; x++) {
            row[x] = 0;
        }
    }
}

#define historysize 16                  //max amount of entries in history
char history[historysize][max_output];  //history index
int historycount = 0;                   //how many entries in history
int historypos = 0;                     //the current point in the history
int historyview = 0;                   //not browsing command history

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
    (void)color;                                    //fixed color
    draw_char(c, x * 8, y * 8, 0xFF0000, 0x000000);  //red on black
}

void terminal_scrolldown() {
    //repeat for each row of pixels
    for (uint32_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (uint32_t py = 0; py < 8; py++) {
            uint8_t* dst = (uint8_t*)fb + (y * 8 + py) * fb_pitch;
            uint8_t* src = (uint8_t*)fb + ((y + 1) * 8 + py) * fb_pitch;
            for (uint32_t x = 0; x < fb_width * fb_bpp; x++) {
                dst[x] = src[x];
            }
        }
    }
    // clear last row
    for(uint32_t y = (VGA_HEIGHT - 1) * 8; y < VGA_HEIGHT * 8; y++) {
        uint8_t* row = (uint8_t*)fb + y * fb_pitch;
        for(uint32_t x = 0; x < fb_width * fb_bpp; x++) {
            row[x] = 0;
        }
    }


    terminal_row = VGA_HEIGHT - 1;
}

//disable cursor
void disable_textcursor() {
    outb(0x3D4, 0x0A);
	outb(0x3D5, 0x20);
}

//make argument works like in echo for exmaple

int getarg_index(const char* cmd) {
    size_t cmd_len = strlen(cmd);
    //check if buffer longer than command and the space after it
    if(output_index <= cmd_len) {
        return -1;  
    }
    //check if command matches
    for (size_t i = 0; i < cmd_len; i++) {
        if(out_buffer[i] != cmd[i]) {
            return -1;
        }
    }
    //space again
    if(out_buffer[cmd_len] != ' ') {
        return -1;
    }
    //return index of the argument
    return cmd_len + 1;
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

uint8_t scancode;

void draw_textcursor() {
    static int last_x = -1;
    static int last_y = -1;

    int cursor_x = terminal_column * 8;
    int cursor_y = terminal_row * 8;

    if (cursor_visibility) {
        draw_char('_', cursor_x, cursor_y, 0xFFFFFF, 0x000000);
        last_x = cursor_x;
        last_y = cursor_y;
    } else {
        if (last_x != -1) {
            draw_char(' ', last_x, last_y, 0x000000, 0x000000);
            last_x = -1;
            last_y = -1;
        }
    }
}

void badapple() {
    can_print_char = 0;
    esc_pressed = 0;
    terminal_writestring("\nreminder! ESC to quit!\n");
    sleep(2000);
    //disable cursor
    cursor_visibility = 0;
    cursor_timer = 0;
    draw_textcursor();
    for (int f = 0; f < FRAME_COUNT; f++) {
        draw_frame(frames[f], 1);

        //30 fps
        sleep(33);

        //broken quit functionality
        if(esc_pressed) {
            esc_pressed = 0;
            //go back
            clear_pixels();

            terminal_initialize();
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            terminal_row = 0;
            terminal_column = 0;
            can_print_char = 1;

            //reenable cursor
            cursor_visibility = 1;
            draw_textcursor();

            return;
        }
    }
}

void print_buffer(void) {
    // LIST OF COMMANDS //
    // ---------------------------------
    // hello - writes "hello world!"
    // ---------------------------------
    // clear - clear screen
    // ---------------------------------
    // shutdown - shutdown the machine
    // ---------------------------------
    // reboot - reset the machine
    // ---------------------------------
    // echo * - print text given after command
    // ---------------------------------
    // read * - read a text file
    // ---------------------------------
    // badapple - fun
    // ---------------------------------
    
    
    //simple hello comand
    if(strcmp_buf("hello")) {
        sleep(200);
        terminal_writestring("\nhello world!!\n");
    }


    //clear by basically reinitilizing the terminal
    else if(strcmp_buf("clear")) {
        terminal_initialize();
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    }

    else if(strcmp_buf("shutdown")) {
        terminal_writestring("\nshutting down... goodbye!\n");
        sleep(300);
        outw(0x604, 0x2000); //QEMU method
    }


    //rebooting (not tested on real hardware but works under QEMU) https://wiki.osdev.org/Reboot
    else if(strcmp_buf("reboot")) {
        terminal_writestring("\nrebooting... goodbye!\n");
        sleep(300);
        uint8_t g = 0x02;
        while(g & 0x02)
            g = inb(0x64);
        outb(0x64, 0xFE);
    }

    //print given text
    //this is NOT broken and i bothered to fix it
    //because it takes arguments now les go :>
    else if(getarg_index("echo") != -1) {
        int arg = getarg_index("echo");
        terminal_writestring("\n");
        for(size_t i = arg; i < output_index; i++) {
            terminal_putchar(out_buffer[i]);
        }
        terminal_writestring("\n");
    }


    //read text file
    else if(getarg_index("read") != -1) {
        int arg = getarg_index("read");
        
        //extract filename from buffer and store
        char name[max_files];
        size_t fname_len = 0;
        for (size_t i = arg; i < output_index && fname_len < max_name - 1; i++) {
            name[fname_len++] = out_buffer[i]; //copy every character from filename
        }
        name[fname_len] = '\0'; //null

        //search ramdisk
        int found = 0;
        for (int i = 0; i < max_files; i++) {
            if(ramdisk[i].used && strlen(ramdisk[i].name) == fname_len) {
                //compare name to list of names from ramdisk
                int match = 1;
                for (size_t j = 0; j < fname_len; j++) {
                    if(ramdisk[i].name[j] != name[j]) {
                        match = 0;
                        break;
                    }
                }
                if(match) {
                    //write content if found
                    terminal_writestring("\n");
                    for (size_t j = 0; j < ramdisk[i].size; j++) {
                        terminal_putchar(ramdisk[i].data[j]);
                    }
                    terminal_writestring("\n");
                    found = 1;
                    break;
                }
            }
        }

        if(!found) {
                terminal_writestring("\n");
                terminal_writestring("file not found.");
                terminal_writestring("\n");
            
        }
    }


    //if just echo dont throw unknown tell the user how to use the command
    else if(strcmp_buf("echo")) {
        terminal_writestring("\nincorrect usage, echo *space* words\n");
    }
    //list all files that were successfully in ramdisk
    else if(strcmp_buf("list")) {
        int found = 0;
        //repeat until every file in the list is went through
        for (int i = 0; i < max_files; i++) {
            if (ramdisk[i].used) {
                terminal_writestring("\n");
                terminal_writestring(ramdisk[i].name);
                terminal_writestring("\n");
                found = 1;
            }
        }
        if (!found) {
            terminal_writestring("\nno files\n");
        }
    }

    //do a funny
    else if(strcmp_buf("badapple")) {
        badapple();
        clear_pixels();

        terminal_initialize();
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_row = 0;
        terminal_column = 0;
    }

    //help - list commands
    else if(strcmp_buf("help")) {
        terminal_writestring("\n");
        terminal_writestring("-hello        Prints 'Hello World!'.   |   -clear        Clears screen.\n");
        terminal_writestring("\n");
        terminal_writestring("-shutdown     Power off the machine.   |   -reboot       Reboot the machine.\n");
        terminal_writestring("\n");
        terminal_writestring("-echo *       Print desired text.      |   -read *       Read file from ramdisk.\n");
        terminal_writestring("-list         List files in ramdisk.   |   -badapple     peak animation trust.\n");
    }

    //handle unknown commands
    else {
        terminal_writestring("\nunknown command: ");
        for (size_t i = 0; i < output_index; i++) {
            terminal_putchar(out_buffer[i]);
        }
        terminal_writestring("\n");
        terminal_writestring("protip 101: run help for a list of available commands.");
        terminal_writestring("\n");
    }
}

void keyboard_handler() {
    scancode = inb(0x60);
    outb(0x20, 0x20);

    if(scancode == 0x01) esc_pressed = 1;
    char c = '>';

    //                           //
    //  ALWAYS SET 1 NO DEMENTIA //
    //                           //

    //shift stuff:
    //left press
    if(scancode == 0x2A) {
        shift_pressed = 1;
        return;
    }
    //left release
    if(scancode == 0xAA) {
        shift_pressed = 0;
        return;
    }

    if (scancode & 0x80) {
        return;
    }

    //literally all the letters by scan code according to set 1 in alphabetical order(PS2 only)
    if (scancode == 0x1E) c = shift_pressed ? 'A' : 'a';
    if (scancode == 0x30) c = shift_pressed ? 'B' : 'b';
    if (scancode == 0x2E) c = shift_pressed ? 'C' : 'c';
    if (scancode == 0x20) c = shift_pressed ? 'D' : 'd';
    if (scancode == 0x12) c = shift_pressed ? 'E' : 'e';
    if (scancode == 0x21) c = shift_pressed ? 'F' : 'f';
    if (scancode == 0x22) c = shift_pressed ? 'G' : 'g';
    if (scancode == 0x23) c = shift_pressed ? 'H' : 'h';
    if (scancode == 0x17) c = shift_pressed ? 'I' : 'i';
    if (scancode == 0x24) c = shift_pressed ? 'J' : 'j';
    if (scancode == 0x25) c = shift_pressed ? 'K' : 'k';
    if (scancode == 0x26) c = shift_pressed ? 'L' : 'l';
    if (scancode == 0x32) c = shift_pressed ? 'M' : 'm';
    if (scancode == 0x31) c = shift_pressed ? 'N' : 'n';
    if (scancode == 0x18) c = shift_pressed ? 'O' : 'o';
    if (scancode == 0x19) c = shift_pressed ? 'P' : 'p';
    if (scancode == 0x10) c = shift_pressed ? 'Q' : 'q';
    if (scancode == 0x13) c = shift_pressed ? 'R' : 'r';
    if (scancode == 0x1F) c = shift_pressed ? 'S' : 's';
    if (scancode == 0x14) c = shift_pressed ? 'T' : 't';
    if (scancode == 0x16) c = shift_pressed ? 'U' : 'u';
    if (scancode == 0x2F) c = shift_pressed ? 'V' : 'v';
    if (scancode == 0x11) c = shift_pressed ? 'W' : 'w';
    if (scancode == 0x2D) c = shift_pressed ? 'X' : 'x';
    if (scancode == 0x15) c = shift_pressed ? 'Y' : 'y';
    if (scancode == 0x2C) c = shift_pressed ? 'Z' : 'z';


    //numbers
    if (scancode == 0x0B) c = shift_pressed ? ')' : '0';
    if (scancode == 0x02) c = shift_pressed ? '!' : '1';
    if (scancode == 0x03) c = shift_pressed ? '@' : '2';
    if (scancode == 0x04) c = shift_pressed ? '#' : '3';
    if (scancode == 0x05) c = shift_pressed ? '$' : '4';
    if (scancode == 0x06) c = shift_pressed ? '%' : '5';
    if (scancode == 0x07) c = shift_pressed ? '^' : '6';
    if (scancode == 0x08) c = shift_pressed ? '&' : '7';
    if (scancode == 0x09) c = shift_pressed ? '*' : '8';
    if (scancode == 0x0A) c = shift_pressed ? '(' : '9';

    //other
    if (scancode == 0x34) c = shift_pressed ? '>' : '.';


    //other characters
    /*  tab  */
    if(scancode == 0x0F) {
        //accidentally broke this by making it do nothing,oops
        terminal_writestring("");
    }
    /* space */
    if (scancode == 0x39) c = ' '; 
    /* the slash */
    if (scancode == 0x35) c = shift_pressed ? '?' : '/';
    /* escape */
    if (scancode == 0x01) {
        
    }
    /* enter */
    if (scancode == 0x1C) {
    if (output_index > 0) {
        //add into history
        int x = historycount % historysize;
        for (size_t i = 0; i < output_index; i++) {
            history[x][i] = out_buffer[i];
        }
        history[x][output_index] = '\0';
        historycount++;
        //--------------------
        //make sure the cursor is not there when pressing enter
        cursor_visibility = 0;
        cursor_timer = 0;
        draw_textcursor();
        print_buffer();
        output_index = 0;
        terminal_writestring("@>");
        cursor_visibility = 1;
        draw_textcursor();
    }
    outb(0x20, 0x20);
    return;
    }
    /* backspace*/ if(scancode == 0x0E) {
        if (output_index == 0) {
            outb(0x20, 0x20);
            return;
        }

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
    //arrow key for history
    //up arrow
    if(scancode == 0x48) {
        if(historycount == 0) {
            return;
        }

        if(historyview >= historycount - 1) {
            historyview = historycount - 1;
        } else {
            historyview++;
        }
        int x = historyview;
        //add command to buffer
        output_index = 0;
        for (int i = 0; i < max_output; i++) {
            out_buffer[i] = 0;
        }
        for(int i = 0; history[x][i] && i < max_output - 1; i++) {
            out_buffer[output_index++] = history[x][i];
        }
        out_buffer[output_index] = '\0';
        
        //redraw
        terminal_writestring("\r");
        for (int i = 0; i < VGA_WIDTH; i++) terminal_putchar(' ');
        terminal_writestring("\r");

        for(size_t i = 0; i < output_index; i++) {
            terminal_putchar(out_buffer[i]);
        }
        return;
    }
    //down arrow
    if(scancode == 0x50) {
        if(historycount == 0) {
            return;
        }

        if(historyview <= 0) {
            historyview = 0;
        } else {
            historyview--;
        }
        
        int x = historyview;
        output_index = 0;

        //wipe buffer
        for (int i = 0; i < max_output; i++) {
            out_buffer[i] = 0;
        }
        //load history
        for(int i = 0; history[x][i] && i < max_output - 1; i++) {
            out_buffer[output_index++] = history[x][i];
        }
        out_buffer[output_index] = '\0';
        //redraw
        terminal_writestring("\r");
        for (int i = 0; i < VGA_WIDTH; i++) {
            terminal_putchar(' ');
        }
        terminal_writestring("\r");
        for(size_t i = 0; i < output_index; i++) {
            terminal_putchar(out_buffer[i]);
        }
        return;
    }
    
    if(c != '>') {//ignore if invalid/unconfiged scancode{
        if(can_print_char) {
                //add character to buffer
            if(output_index < max_output) {
                out_buffer[output_index++] = c;
            }
            terminal_putchar(c);
            draw_textcursor();
        }
    }
}

extern void keyboard_isr(void);

extern void loadpagedirectory(uint32_t*);
extern void enablepaging();

void print_hex(uint32_t n) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        int nibble = n & 0xF;
        buf[i] = nibble < 10 ? '0' + nibble : 'a' + (nibble - 10);
        n >>= 4;
    }
    terminal_writestring(buf);
}

void kernel_main(uint32_t m_addr)
{
    struct multiboot* m = (struct multiboot*)m_addr;

    //grab framebuffer
    fb = (uint32_t*)(uint32_t)m->framebuffer_addr;
    fb_pitch = m->framebuffer_pitch;
    fb_width = m->framebuffer_width;
    fb_height = m->framebuffer_height;
    fb_bpp = m->framebuffer_bpp / 8;

    ramdisk = malloc(sizeof(struct file) * max_files);

    initramdisk(m);
    disable_textcursor();
    terminal_initialize();

    /* uint32_t* fb = (uint32_t*)(uint32_t)m->framebuffer_addr;
    uint32_t pitch = m->framebuffer_pitch;
    uint32_t width = m->framebuffer_width;
    uint32_t height = m->framebuffer_height; */

    /*for(uint32_t y = 0; y < 100; y++) {
            for(uint32_t x = 0; x < 100; x++) {
                uint32_t* pixel = (uint32_t*)((uint8_t*)fb + y * pitch + x * 4);
                *pixel = 0x00FF0000;   red 
            }
        
    */

    //paging/memory stuff
    blank_pagedir();                    //blank everything in the page directory
    //create_table((uint32_t)m->framebuffer_addr);                     //create table
    //loadpagedirectory(page_directory);  //load new page directory
    //enablepaging();                     //enable paging

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    terminal_writestring("hello!\nthis is a new line..\n");
    terminal_writestring("its alive!!\n");


    terminal_writestring("@>");
    cursor_visibility = 1;
    draw_textcursor();
    can_print_char = 1;
    pic_remap();
    extern void default_isr(void);
    for (int i = 0; i < 256; i++)
        set_idt_entry(i, (uint32_t)default_isr);

    set_idt_entry(33, (uint32_t)keyboard_isr);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint32_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    extern void timer_isr(void);
    set_idt_entry(32, (uint32_t)timer_isr);
    initpit(1000);
    initpit(1000);
    __asm__ volatile ("sti");
    for(;;) __asm__("hlt");
}