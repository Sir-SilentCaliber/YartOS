/* Yart OS - icon API backed by real raster assets generated at build time.
 * The actual pixel data lives in asset_icons.c. */
#include <yart/icons.h>
#include <yart/gui.h>

typedef struct { const u32 *px; int w; int h; } icon_asset_t;
extern const icon_asset_t yart_icons[];
extern const int yart_icons_count;

const u32 *icon_pixels(icon_id_t id) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return 0;
    return yart_icons[id].px;
}

/* Blend a single ARGB source pixel onto the backbuffer at (x,y). */
static inline void put_argb(int x, int y, u32 src) {
    u32 a = (src >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 0xFF) { draw_pixel(x, y, src); return; }
    extern fb_ctx_t g_fb;
    if ((u32)x >= g_fb.width || (u32)y >= g_fb.height) return;
    u32 *dst = &g_fb.pixels[y * g_fb.pitch_px + x];
    u32 d = *dst;
    u32 sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    u32 dr = (d   >> 16) & 0xFF, dg = (d   >> 8) & 0xFF, db = d   & 0xFF;
    u32 ia = 255 - a;
    u32 nr = (sr * a + dr * ia) >> 8;
    u32 ng = (sg * a + dg * ia) >> 8;
    u32 nb = (sb * a + db * ia) >> 8;
    *dst = 0xFF000000U | (nr << 16) | (ng << 8) | nb;
}

void draw_icon(int x, int y, icon_id_t id) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++)
            put_argb(x + i, y + j, a->px[j * a->w + i]);
}

void draw_icon_scaled(int x, int y, icon_id_t id, int scale) {
    if (scale <= 1) { draw_icon(x, y, id); return; }
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    for (int j = 0; j < a->h; j++)
        for (int i = 0; i < a->w; i++) {
            u32 c = a->px[j * a->w + i];
            if (!(c & 0xFF000000)) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    put_argb(x + i * scale + sx, y + j * scale + sy, c);
        }
}

void draw_icon_sized(int x, int y, icon_id_t id, int size) {
    if ((int)id < 0 || (int)id >= yart_icons_count) return;
    const icon_asset_t *a = &yart_icons[id];
    if (size <= 0) return;
    for (int j = 0; j < size; j++) {
        int sy = j * a->h / size;
        for (int i = 0; i < size; i++) {
            int sx = i * a->w / size;
            put_argb(x + i, y + j, a->px[sy * a->w + sx]);
        }
    }
}

/* ---------- icon_for_file: pick icon by extension ---------- */
#include <yart/string.h>

static bool ends_with(const char *name, const char *ext) {
    int nl = (int)strlen(name), el = (int)strlen(ext);
    if (el > nl) return false;
    const char *tail = name + nl - el;
    for (int i = 0; i < el; i++) {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

icon_id_t icon_for_file(const char *name, int is_dir) {
    if (is_dir) {
        /* Folder name-based detection */
        if (strcmp(name, "Pictures") == 0 || strcmp(name, "pictures") == 0
            || strcmp(name, "Images") == 0)  return ICON_FOLDER_PIC;
        if (strcmp(name, "Music") == 0   || strcmp(name, "music") == 0)    return ICON_FOLDER_MUSIC;
        if (strcmp(name, "Videos") == 0  || strcmp(name, "videos") == 0)   return ICON_FOLDER_VIDEO;
        if (strcmp(name, "Documents") == 0|| strcmp(name, "documents") == 0) return ICON_FOLDER_DOC;
        if (strcmp(name, "Downloads") == 0|| strcmp(name, "downloads") == 0) return ICON_FOLDER_DL;
        if (strcmp(name, "Desktop") == 0 || strcmp(name, "desktop") == 0)  return ICON_FOLDER;
        return ICON_FOLDER;
    }
    /* Image files */
    if (ends_with(name, ".png")  || ends_with(name, ".jpg")  ||
        ends_with(name, ".jpeg") || ends_with(name, ".bmp")  ||
        ends_with(name, ".gif")  || ends_with(name, ".svg")  ||
        ends_with(name, ".webp") || ends_with(name, ".ico"))
        return ICON_IMG;
    /* Video files */
    if (ends_with(name, ".mp4")  || ends_with(name, ".avi")  ||
        ends_with(name, ".mkv")  || ends_with(name, ".mov")  ||
        ends_with(name, ".webm") || ends_with(name, ".flv")  ||
        ends_with(name, ".wmv"))
        return ICON_VIDEO;
    /* Music files */
    if (ends_with(name, ".mp3")  || ends_with(name, ".wav")  ||
        ends_with(name, ".ogg")  || ends_with(name, ".flac") ||
        ends_with(name, ".aac")  || ends_with(name, ".wma")  ||
        ends_with(name, ".mid")  || ends_with(name, ".midi"))
        return ICON_MUSIC;
    /* Source code files */
    if (ends_with(name, ".c")    || ends_with(name, ".h")    ||
        ends_with(name, ".cpp")  || ends_with(name, ".hpp")  ||
        ends_with(name, ".py")   || ends_with(name, ".js")   ||
        ends_with(name, ".ts")   || ends_with(name, ".rs")   ||
        ends_with(name, ".go")   || ends_with(name, ".java") ||
        ends_with(name, ".asm")  || ends_with(name, ".s")    ||
        ends_with(name, ".sh")   || ends_with(name, ".rb")   ||
        ends_with(name, ".pl")   || ends_with(name, ".lua")  ||
        ends_with(name, ".cs")   || ends_with(name, ".swift")||
        ends_with(name, ".kt")   || ends_with(name, ".zig")  ||
        ends_with(name, ".ld")   || ends_with(name, ".cfg")  ||
        ends_with(name, ".mk")   || ends_with(name, ".toml") ||
        ends_with(name, ".json") || ends_with(name, ".xml")  ||
        ends_with(name, ".yaml") || ends_with(name, ".yml"))
        return ICON_CODE;
    /* Archive files */
    if (ends_with(name, ".zip")  || ends_with(name, ".tar")  ||
        ends_with(name, ".gz")   || ends_with(name, ".bz2")  ||
        ends_with(name, ".xz")   || ends_with(name, ".7z")   ||
        ends_with(name, ".rar")  || ends_with(name, ".tgz"))
        return ICON_ARCHIVE;
    /* Text / document files */
    if (ends_with(name, ".txt")  || ends_with(name, ".md")   ||
        ends_with(name, ".log")  || ends_with(name, ".ini")  ||
        ends_with(name, ".conf") || ends_with(name, ".rtf")  ||
        ends_with(name, ".csv"))
        return ICON_TEXT;
    return ICON_FILE;
}
