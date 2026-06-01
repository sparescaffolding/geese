### geese
simple x86 OS made by me written in C with a linear framebuffer and basic ramdisk system.

### features:
- linear framebuffer running at 640x480 with a bitmap font
- ramdisk system loaded via GRUB modules (loads files from /iso/boot/root/)
- the hit bad apple animation via the ``badapple`` command

### list of commands:
| command | description |
|---|---|
| `hello` | prints "Hello World!" |
| `clear` | clears the screen |
| `shutdown` | powers off the machine (QEMU only) |
| `reboot` | reboots the machine |
| `echo *` | prints desired text |
| `read *` | reads a file from ramdisk |
| `list` | lists files in ramdisk |
| `badapple` | peak animation trust |
| `help` | lists currently available commands |

# build instructions
if you want to run this OS fancy without grabbing the image from releases, below is an all in one command that does it all!
requires build-essential, xorriso, grub-pc-bin, grub-common and i686-elf cross compiler of your choice!
```
i686-elf-as boot.s -o boot.o && i686-elf-gcc -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra && i686-elf-gcc -c badapple.c -o badapple.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra && i686-elf-gcc
-T linker.ld -o os -ffreestanding -O2 -nostdlib boot.o kernel.o badapple.o -lgcc && cp os iso/boot/ && cp grub.cfg iso/boot/grub/ && grub-mkrescue -o geese.iso iso
```
