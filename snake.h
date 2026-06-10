#ifndef SNAKE_H
#define SNAKE_H

#include <stdint.h>
#include <libdragon.h>

bool snake_is_active(void);
void snake_stop(void);
void snake_toggle(void);
void snake_handle_input(joypad_buttons_t pressed);
void snake_render(surface_t *disp, bool has_progress,
    uint32_t progress_pct, uint32_t errors);

#endif
