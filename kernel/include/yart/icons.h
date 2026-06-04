#pragma once
#include <yart/types.h>

#define ICON_W 32
#define ICON_H 32

typedef enum {
    /* Original app/system icons */
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

    /* File-type icons */
    ICON_IMG,           /* image / picture file */
    ICON_VIDEO,         /* video file */
    ICON_CODE,          /* source code file */
    ICON_MUSIC,         /* audio / music file */
    ICON_ARCHIVE,       /* zip / archive file */
    ICON_TEXT,          /* plain text file */

    /* Folder variant icons */
    ICON_FOLDER_OPEN,
    ICON_FOLDER_PIC,
    ICON_FOLDER_MUSIC,
    ICON_FOLDER_VIDEO,
    ICON_FOLDER_DOC,
    ICON_FOLDER_DL,

    /* System icons */
    ICON_RECYCLE,
    ICON_NETWORK,
    ICON_LOCK,

    ICON_COUNT
} icon_id_t;

/* Pick an icon for a file based on its extension */
icon_id_t icon_for_file(const char *name, int is_dir);

const u32 *icon_pixels(icon_id_t id);
void       draw_icon(int x, int y, icon_id_t id);
void       draw_icon_sized(int x, int y, icon_id_t id, int size);
void       draw_icon_scaled(int x, int y, icon_id_t id, int scale);
