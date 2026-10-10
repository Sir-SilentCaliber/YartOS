/* ---- system clipboard (cross-process copy/paste, 512 bytes) ---- */
#define CLIP_MAX 512
static char  g_clipboard[CLIP_MAX];
static int   g_clipboard_len;
static spinlock_t g_clipboard_lock;

static i64 sys_clipboard_set(const char *text) {
    char kb[CLIP_MAX];
    if (!copy_user_str((u64)text, kb, sizeof kb)) return -1;
    u64 fl = irq_save();
    spin_lock(&g_clipboard_lock);
    int n = 0;
    while (kb[n] && n < CLIP_MAX - 1) n++;
    memcpy(g_clipboard, kb, n);
    g_clipboard[n] = 0;
    g_clipboard_len = n;
    spin_unlock(&g_clipboard_lock);
    irq_restore(fl);
    return 0;
}

static i64 sys_clipboard_get(char *out, u64 cap) {
    if (!uptr((u64)out, cap)) return -1;
    u64 fl = irq_save();
    spin_lock(&g_clipboard_lock);
    u64 n = (u64)g_clipboard_len;
    if (n >= cap) n = cap ? cap - 1 : 0;
    stac();
    for (u64 i = 0; i < n; i++) out[i] = g_clipboard[i];
    if (cap) out[n] = 0;
    clac();
    spin_unlock(&g_clipboard_lock);
    irq_restore(fl);
    return (i64)g_clipboard_len;
}
