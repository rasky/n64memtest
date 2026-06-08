#ifndef LOGO_H
#define LOGO_H

#include <stdint.h>
#include <libdragon.h>

void logo_draw(surface_t *disp, int center_x, int center_y, uint32_t color,
               uint32_t ticks_ms, int analog_x, int analog_y);

#endif
