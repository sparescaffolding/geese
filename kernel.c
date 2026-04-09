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

struct multiboot {
    uint32_t flags;
    uint32_t memlow;
    uint32_t memup;
    uint32_t boot_dev;
    uint32_t cmd;
    uint32_t mods_loaded;
    uint32_t mods_address;
};

struct multimod {
    uint32_t start;
    uint32_t end;
    uint32_t cmd;
    uint32_t reserved;
};

#define max_files 8
#define max_name 16
#define max_size 1024

struct file {
    char name[max_name];
    char data[max_size];
    size_t size;
    int used; //1 if there is a file
};

struct file ramdisk[max_files];

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

uint32_t page_directory[1024] __attribute__((aligned(4096)));
uint32_t first_table[1024] __attribute__((aligned(4096)));

//blank page directory
void blank_pagedir() {
    int i;
    for(i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002;
    }
}

void create_table() {
    unsigned int i;
    //4 megabyte
    for(i = 0; i < 1024; i++) {
        first_table[i] = (i * 0x1000) | 3;
    }
    //put the page into directory
    page_directory[0] = ((unsigned int)first_table) | 3;
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
#define VGA_HEIGHT  25
#define VGA_MEMORY  0xB8000

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer = (uint16_t*)VGA_MEMORY;

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
    
    //simple hello comand
    if(strcmp_buf("hello")) {
        terminal_writestring("\nhello world!!\n");
    }


    //clear by basically reinitilizing the terminal
    else if(strcmp_buf("clear")) {
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
        for (int i = 0; i < name; i++) {
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
                    for(size_t j = 0; j < ramdisk[i].data[j]; j++) {
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

    //other
    if (scancode == 0x34) c = '.';



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
        terminal_writestring("@>");
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

extern void loadpagedirectory(unsigned int*);
extern void enablepaging();

void kernel_main(uint32_t m_addr)
{
    struct multiboot* m = (struct multiboot*)m_addr;
    initramdisk(m);
    disable_textcursor();
    terminal_initialize();

    //paging/memory stuff
    blank_pagedir();                    //blank everything in the page directory
    create_table();                     //create table
    loadpagedirectory(page_directory);  //load new page directory
    enablepaging();                     //enable paging

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