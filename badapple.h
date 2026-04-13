#ifndef BADAPPLE_H
#define BADAPPLE_H

#include <stdint.h>

#define WIDTH 128
#define HEIGHT 96
#define FRAME_COUNT 9721

extern uint8_t frames[FRAME_COUNT][(WIDTH*HEIGHT)/8];

#endif