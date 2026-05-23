#pragma once
#include <yart/types.h>

#define ICON_W 32
#define ICON_H 32

typedef enum {
    ICON_FILES = 0,
    ICON_TERM,
    ICON_EDITOR,
    ICON_CLOCK,
    ICON_INFO,
    ICON_FOLDER,
    ICON_FILE,
    ICON_HOME,
    ICON_CONFIG,
    ICON_DRAWER,
    ICON_CALC,
    ICON_MONITOR,
    ICON_COUNT
} icon_id_t;

const u32 *icon_pixels(icon_id_t id);
void       draw_icon(int x, int y, icon_id_t id);
void       draw_icon_sized(int x, int y, icon_id_t id, int size);
void       draw_icon_scaled(int x, int y, icon_id_t id, int scale);
