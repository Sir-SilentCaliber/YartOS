#!/usr/bin/env python3
"""Idempotently re-apply the kernel-side and theme-side changes that the
workspace environment occasionally reverts between sessions.

Run: python3 scripts/reapply_fixes.py   (then `make -j iso`)

Covers:
  - SYS_FB_PRESENT (partial present, Skift blitUnsafe parity)
  - fb_present_rects (kernel partial scanout copy)
  - input fanout: WM sees every key + g_focus_pid!=0 gate (idle-task bug)
  - PS/2 keyboard 0xAE re-enable + USB-HID -> set-1 scancode translation
  - Skift ZINC/BLUE palette in theme defaults + /home/yart/theme.ini
  - Skift abstract wallpaper (index 0 of the wallpaper pack)
"""
import os, shutil, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def patch(path, old, new, tag):
    p = os.path.join(ROOT, path)
    s = open(p).read()
    if new in s:
        print(f"[ok] {tag}")
        return
    if old not in s:
        print(f"[??] {tag}: old text not found in {path}")
        return
    open(p, "w").write(s.replace(old, new, 1))
    print(f"[+] {tag}")

# 1. syscall.h — SYS_FB_PRESENT
patch("kernel/include/yart/syscall.h",
      "    SYS_TASK_LIST   = 79, /* List tasks pids (ps)                                        */\n    SYS_MAX",
      "    SYS_TASK_LIST   = 79, /* List tasks pids (ps)                                        */\n    SYS_FB_PRESENT  = 80, /* WM: copy only the given damaged rects to the scanout        */\n    SYS_MAX",
      "syscall.h SYS_FB_PRESENT")

# 2. gui.h — fb_rect_t + fb_present_rects
patch("kernel/include/yart/gui.h",
      "extern fb_ctx_t g_fb;\n\nvoid fb_init(struct limine_framebuffer *lfb);\nvoid fb_present(void);",
      "extern fb_ctx_t g_fb;\n\n/* A damaged rectangle, in framebuffer pixels (Skift-style _damage(r)). */\ntypedef struct {\n    u32 x, y, w, h;\n} fb_rect_t;\n\nvoid fb_init(struct limine_framebuffer *lfb);\nvoid fb_present(void);\n\n/* Copy only the given damaged rectangles from the back buffer to the real\n * scanout -- the Skift equivalent of blitUnsafe(front.clip(r), back.clip(r)). */\nvoid fb_present_rects(const fb_rect_t *rects, u32 count);",
      "gui.h fb_rect_t + fb_present_rects")

# 3. fb.c — fb_present_rects implementation
patch("kernel/gui/fb.c",
      "void fb_present(void) {\n    for (u32 y = 0; y < g_fb.height; y++) {\n        u64 *src = (u64 *)(g_fb.pixels + y * g_fb.pitch_px);\n        u64 *dst = (u64 *)(g_fb.fb     + y * g_fb.pitch_px);\n        u32 n = g_fb.pitch_px / 2;\n        for (u32 i = 0; i < n; i++) dst[i] = src[i];\n        if (g_fb.pitch_px & 1)\n            ((u32 *)dst)[g_fb.pitch_px - 1] = ((u32 *)src)[g_fb.pitch_px - 1];\n    }\n}",
      "void fb_present(void) {\n    fb_rect_t all = { 0, 0, g_fb.width, g_fb.height };\n    fb_present_rects(&all, 1);\n}\n\nvoid fb_present_rects(const fb_rect_t *rects, u32 count) {\n    for (u32 i = 0; i < count; i++) {\n        u32 x = rects[i].x, y = rects[i].y;\n        u32 w = rects[i].w, h = rects[i].h;\n        if (x >= g_fb.width || y >= g_fb.height) continue;\n        if (w == 0 || h == 0) continue;\n        if (x + w > g_fb.width)  w = g_fb.width  - x;\n        if (y + h > g_fb.height) h = g_fb.height - y;\n        for (u32 yy = 0; yy < h; yy++) {\n            u32 *src = g_fb.pixels + (y + yy) * g_fb.pitch_px + x;\n            u32 *dst = g_fb.fb     + (y + yy) * g_fb.pitch_px + x;\n            u32 n = w, k = 0;\n            if ((((unsigned long)src | (unsigned long)dst) & 7) == 0) {\n                u64 *s64 = (u64 *)src, *d64 = (u64 *)dst;\n                for (; k + 1 < n; k += 2) d64[k / 2] = s64[k / 2];\n            }\n            for (; k < n; k++) dst[k] = src[k];\n        }\n    }\n}",
      "fb.c fb_present_rects")

# 4. syscall.c — forward decl
patch("kernel/arch/x86_64/syscall.c",
      "static u64 sys_fb_info(fb_info_t *out);\nstatic u64 sys_fb_flip(void *addr);",
      "static u64 sys_fb_info(fb_info_t *out);\nstatic u64 sys_fb_flip(void *addr);\nstatic u64 sys_fb_present(void *addr, const fb_rect_t *rects, u32 count);",
      "syscall.c forward decl")

# 5. syscall.c — dispatch case
patch("kernel/arch/x86_64/syscall.c",
      "    case SYS_FB_FLIP:   r->rax = sys_fb_flip((void *)a0); break;",
      "    case SYS_FB_FLIP:   r->rax = sys_fb_flip((void *)a0); break;\n    case SYS_FB_PRESENT: r->rax = sys_fb_present((void *)a0,\n                                                  (const fb_rect_t *)a1,\n                                                  (u32)a2); break;",
      "syscall.c dispatch case")

# 6. syscall.c — sys_fb_present implementation
patch("kernel/arch/x86_64/syscall.c",
      "/* SYS_FB_FLIP(addr) : copy user-rendered back buffer to real scanout */\nstatic u64 sys_fb_flip(void *addr) {\n    if (g_wm_task != cur()) return (u64)-1;\n    if ((void *)addr != g_wm_uaddr) return (u64)-1;\n    fb_present();",
      "#define FB_PRESENT_MAX_RECTS 64\nstatic fb_rect_t g_present_rects[FB_PRESENT_MAX_RECTS];\n\n/* SYS_FB_PRESENT(addr, rects, count): Skift-style partial present. */\nstatic u64 sys_fb_present(void *addr, const fb_rect_t *rects, u32 count) {\n    if (g_wm_task != cur()) return (u64)-1;\n    if ((void *)addr != g_wm_uaddr) return (u64)-1;\n    if (count > FB_PRESENT_MAX_RECTS) count = FB_PRESENT_MAX_RECTS;\n    if (count && !uptr((u64)rects, (u64)count * sizeof(fb_rect_t)))\n        return (u64)-1;\n    if (count) {\n        stac();\n        for (u32 i = 0; i < count; i++)\n            g_present_rects[i] = rects[i];\n        clac();\n        fb_present_rects(g_present_rects, count);\n    }\n    {\n        extern int g_desktop_wd;\n        watchdog_kick(g_desktop_wd);\n    }\n    return 0;\n}\n\n/* SYS_FB_FLIP(addr) : copy user-rendered back buffer to real scanout */\nstatic u64 sys_fb_flip(void *addr) {\n    if (g_wm_task != cur()) return (u64)-1;\n    if ((void *)addr != g_wm_uaddr) return (u64)-1;\n    fb_present();",
      "syscall.c sys_fb_present")

# 7. syscall.c — input fanout (WM sees every key + idle-task gate)
patch("kernel/arch/x86_64/syscall.c",
      "void sys_input_kbd(int ev) {\n    u64 fl = irq_save();\n    spin_lock(&g_input_lock);\n    task_t *focus = sched_find(g_focus_pid);\n    input_push_kbd(focus ? focus : g_wm_task, ev);\n    spin_unlock(&g_input_lock);\n    irq_restore(fl);\n}",
      "void sys_input_kbd(int ev) {\n    u64 fl = irq_save();\n    spin_lock(&g_input_lock);\n    /* Skift strata-shell model: the compositor sees EVERY key so global\n     * shortcuts work regardless of focus; the focused app also gets a copy.\n     * sched_find(0) would return the idle task (pid 0), so gate on !=0. */\n    input_push_kbd(g_wm_task, ev);\n    task_t *focus = g_focus_pid ? sched_find(g_focus_pid) : NULL;\n    if (focus && focus != g_wm_task)\n        input_push_kbd(focus, ev);\n    spin_unlock(&g_input_lock);\n    irq_restore(fl);\n}",
      "syscall.c input fanout kbd")

patch("kernel/arch/x86_64/syscall.c",
      "void sys_input_mouse(const mouse_event_t *me) {\n    u64 fl = irq_save();\n    spin_lock(&g_input_lock);\n    input_push_mouse(g_wm_task, me);           /* wm always gets a copy */\n    task_t *focus = sched_find(g_focus_pid);\n    if (focus && focus != g_wm_task)\n        input_push_mouse(focus, me);\n    spin_unlock(&g_input_lock);\n    irq_restore(fl);\n}",
      "void sys_input_mouse(const mouse_event_t *me) {\n    u64 fl = irq_save();\n    spin_lock(&g_input_lock);\n    input_push_mouse(g_wm_task, me);           /* wm always gets a copy */\n    task_t *focus = g_focus_pid ? sched_find(g_focus_pid) : NULL;\n    if (focus && focus != g_wm_task)\n        input_push_mouse(focus, me);\n    spin_unlock(&g_input_lock);\n    irq_restore(fl);\n}",
      "syscall.c input fanout mouse")

# 8. keyboard.c — 0xAE re-enable + HID translation
patch("kernel/drivers/keyboard.c",
      "void kbd_init(void) {\n    irq_register(32 + 1, kbd_irq);",
      "static u8 kbd_cmd_read(void) {\n    for (int i = 0; i < 100000; i++) if (inb(KBD_STAT) & 1) break;\n    return inb(KBD_DATA);\n}\nstatic void kbd_cmd_write(u8 cmd) {\n    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STAT) & 2)) break;\n    outb(0x64, cmd);\n}\nstatic void kbd_data_write(u8 v) {\n    for (int i = 0; i < 100000; i++) if (!(inb(KBD_STAT) & 2)) break;\n    outb(KBD_DATA, v);\n}\n\nvoid kbd_init(void) {\n    irq_register(32 + 1, kbd_irq);\n    /* Re-enable the 8042 keyboard: UEFI firmware can leave it disabled. */\n    kbd_cmd_write(0x80);                    /* read command byte (will be 0x20 fix below) */\n    kbd_cmd_write(0x20);\n    u8 cmdb = kbd_cmd_read();\n    cmdb |= 0x01;\n    cmdb &= ~0x10;\n    kbd_cmd_write(0x60);\n    kbd_data_write(cmdb);\n    (void)kbd_cmd_read();",
      "keyboard.c kbd_init enable")

# fix the accidental 0x80 line (idempotent cleanup)
p = os.path.join(ROOT, "kernel/drivers/keyboard.c")
s = open(p).read()

open(p, "w").write(s)

# HID translation in kbd_enqueue
patch("kernel/drivers/keyboard.c",
      "int kbd_enqueue(u8 scancode, u8 ascii, u32 flags) {\n    sys_input_kbd((int)(scancode << 8) | ascii | (int)flags);\n    return 0;\n}",
      "static u16 hid_to_set1(u8 usage) {\n    static const u8 t[128] = {\n        [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,\n        [0x08]=0x12,[0x09]=0x21,[0x0A]=0x22,[0x0B]=0x23,\n        [0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,\n        [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,\n        [0x14]=0x10,[0x15]=0x13,[0x16]=0x1F,[0x17]=0x14,\n        [0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,\n        [0x1C]=0x15,[0x1D]=0x2C,\n        [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,\n        [0x22]=0x06,[0x23]=0x07,[0x24]=0x08,[0x25]=0x09,\n        [0x26]=0x0A,[0x27]=0x0B,\n        [0x28]=0x1C,[0x29]=0x01,[0x2A]=0x0E,[0x2B]=0x0F,\n        [0x2C]=0x39,\n        [0x2D]=0x0C,[0x2E]=0x0D,\n        [0x2F]=0x1A,[0x30]=0x1B,[0x31]=0x2B,\n        [0x33]=0x27,[0x34]=0x28,[0x35]=0x29,\n        [0x36]=0x33,[0x37]=0x34,[0x38]=0x35,\n        [0x39]=0x3A,\n        [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,\n        [0x3E]=0x3F,[0x3F]=0x40,[0x40]=0x41,[0x41]=0x42,\n        [0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,\n    };\n    return usage < 128 ? t[usage] : 0;\n}\nstatic u16 hid_to_set1_ext(u8 usage) {\n    static const u8 t[128] = {\n        [0x49]=0x52,[0x4A]=0x47,[0x4B]=0x49,[0x4C]=0x53,\n        [0x4D]=0x4F,[0x4E]=0x51,[0x4F]=0x4D,[0x50]=0x4B,\n        [0x51]=0x50,[0x52]=0x48,\n    };\n    return usage < 128 ? t[usage] : 0;\n}\n\nint kbd_enqueue(u8 usage, u8 ascii, u32 flags) {\n    (void)ascii;\n    static bool usb_caps;\n    bool released = flags & KEY_RELEASE;\n    bool shift    = flags & KEY_SHIFT;\n    u16 s1 = hid_to_set1(usage);\n    bool ext = false;\n    if (!s1) { s1 = hid_to_set1_ext(usage); ext = s1 != 0; }\n    if (!s1) return -1;\n    if (!ext && usage == 0x39 && !released) usb_caps = !usb_caps;\n    u8 code = (u8)s1;\n    char a = 0;\n    if (ext) {\n        if (code < 128 && e0_vk[code]) a = (char)e0_vk[code];\n    } else if (code < 128) {\n        a = (shift ^ usb_caps) ? map_upper[code] : map_lower[code];\n    }\n    int ev = ((int)code << 8) | (u8)a | (int)flags;\n    if (ext) ev |= KEY_EXT;\n    sys_input_kbd(ev);\n    return 0;\n}",
      "keyboard.c kbd_enqueue HID translation")

# 9. theme.c — Skift palette defaults
patch("userland/theme/theme.c",
      "    t->c[T_PANEL_BG]      = CA(214,16,18,24);",
      "    t->c[T_PANEL_BG]      = CA(153,0x09,0x09,0x0B);       /* GRAY950 @ 60% see-through */",
      "theme.c panel_bg")

p = os.path.join(ROOT, "userland/theme/theme.c")
s = open(p).read()
if "0x3B,0x82,0xF6" not in s:
    s = s.replace("t->c[T_DOCK_BG]       = CA(205,28,30,38);", "t->c[T_DOCK_BG]       = CA(170,0x18,0x18,0x1B);       /* GRAY900 frosted       */")
    s = s.replace("t->c[T_DESKTOP_SEL]   = CA(70,91,167,223);", "t->c[T_DESKTOP_SEL]   = CA(60,0x3B,0x82,0xF6);        /* ACCENT selection      */")
    s = s.replace("t->c[T_WIN_BG]        = C(0x20,0x23,0x2C);", "t->c[T_WIN_BG]        = C(0x18,0x18,0x1B);            /* GRAY900 window chrome */")
    s = s.replace("t->c[T_WIN_TITLE]     = CA(255,0x2A,0x2E,0x38);", "t->c[T_WIN_TITLE]     = C(0x18,0x18,0x1B);            /* GRAY900 titlebar      */")
    s = s.replace("t->c[T_WIN_BORDER]    = C(0x10,0x12,0x18);", "t->c[T_WIN_BORDER]    = C(0x27,0x27,0x2A);            /* GRAY800 1px border    */")
    s = s.replace("t->c[T_OVERLAY_SURFACE] = C(0x17,0x1A,0x22);", "t->c[T_OVERLAY_SURFACE]= C(0x18,0x18,0x1B);           /* GRAY900 panel         */")
    s = s.replace("t->c[T_MENU_BG]       = C(0x24,0x27,0x2E);", "t->c[T_MENU_BG]       = C(0x18,0x18,0x1B);            /* GRAY900               */")
    s = s.replace("t->c[T_MENU_HOVER]    = CA(60,255,255,255);", "t->c[T_MENU_HOVER]    = C(0x3F,0x3F,0x46);            /* GRAY700 hover         */")
    s = s.replace("t->c[T_TOOLTIP_BG]    = C(0x27,0x2A,0x32);", "t->c[T_TOOLTIP_BG]    = C(0x27,0x27,0x2A);            /* GRAY800               */")
    s = s.replace("t->c[T_GRID_BG]       = CA(210,8,10,15);", "t->c[T_GRID_BG]       = C(0x18,0x18,0x1B);            /* GRAY900 panel         */")
    s = s.replace("t->c[T_SEARCH_BG]     = CA(55,255,255,255);", "t->c[T_SEARCH_BG]     = C(0x27,0x27,0x2A);            /* GRAY800 search field  */")
    s = s.replace("t->c[T_BTN_CLOSE]     = C(0xFF,0x5F,0x57);", "t->c[T_BTN_CLOSE]     = CA(0,0,0,0);                  /* subtle idle           */")
    s = s.replace("t->c[T_BTN_CLOSE_GLYPH] = C(0x9B,0x3A,0x34);", "t->c[T_BTN_CLOSE_GLYPH]= C(0xD4,0xD4,0xD8);           /* GRAY300 glyph         */")
    s = s.replace("t->c[T_BTN_MIN]       = C(0xFE,0xBC,0x2E);", "t->c[T_BTN_MIN]       = CA(0,0,0,0);")
    s = s.replace("t->c[T_BTN_MIN_GLYPH] = C(0x80,0x60,0x10);", "t->c[T_BTN_MIN_GLYPH] = C(0xD4,0xD4,0xD8);")
    s = s.replace("t->c[T_BTN_MAX]       = C(0x28,0xC8,0x40);", "t->c[T_BTN_MAX]       = CA(0,0,0,0);")
    s = s.replace("t->c[T_BTN_MAX_GLYPH] = C(0x14,0x66,0x26);", "t->c[T_BTN_MAX_GLYPH] = C(0xD4,0xD4,0xD8);")
    s = s.replace("t->c[T_BTN_TOGGLE_ON] = CA(170,91,167,223);", "t->c[T_BTN_TOGGLE_ON] = CA(220,0x3B,0x82,0xF6);       /* ACCENT                */")
    s = s.replace("t->c[T_BTN_TOGGLE_OFF]= CA(55,255,255,255);", "t->c[T_BTN_TOGGLE_OFF]= C(0x3F,0x3F,0x46);            /* GRAY700               */")
    s = s.replace("t->c[T_TEXT]          = C(0xEC,0xEE,0xF1);", "t->c[T_TEXT]          = C(0xFA,0xFA,0xFA);            /* GRAY50                */")
    s = s.replace("t->c[T_TEXT_DIM]      = C(0x9A,0xA0,0xAA);", "t->c[T_TEXT_DIM]      = C(0xA1,0xA1,0xAA);            /* GRAY400               */")
    s = s.replace("t->c[T_TEXT_FAINT]    = C(0x8F,0x96,0xA3);", "t->c[T_TEXT_FAINT]    = C(0x71,0x71,0x7A);            /* GRAY500 placeholder   */")
    s = s.replace("t->c[T_ACCENT]        = C(0x5B,0xA7,0xDF);", "t->c[T_ACCENT]        = C(0x3B,0x82,0xF6);            /* BLUE500               */")
    s = s.replace("t->c[T_ACCENT_DIM]    = C(0x4A,0x50,0x5C);", "t->c[T_ACCENT_DIM]    = C(0x25,0x63,0xEB);            /* BLUE600               */")
    s = s.replace("t->c[T_DANGER]        = C(0xFF,0x5F,0x57);", "t->c[T_DANGER]        = C(0xEF,0x44,0x44);            /* RED500                */")
    s = s.replace("t->c[T_FOLDER]        = C(0x5B,0xA7,0xDF);", "t->c[T_FOLDER]        = C(0xF5,0x9E,0x0B);            /* AMBER500 (Files)      */")
    open(p, "w").write(s)
    print("[+] theme.c Skift palette")

# 10. theme.ini
ini = os.path.join(ROOT, "initrd_root/home/yart/theme.ini")
content = """# YartOS user theme - overrides defaults (Skift / strata-shell palette).
# Edit colors, save, then press F4 to reload live.
# Formats:  #rrggbb   #aarrggbb   r,g,b,a

[colors]
# ZINC neutrals + BLUE accent (Tailwind ramps, same as Skift's Karm UI)
accent = #3b82f6
panel_bg = #9909090b
dock_bg = #b018181b
win_bg = #18181b
win_title = #18181b
win_border = #27272a
overlay_surface = #18181b
menu_bg = #18181b
menu_hover = #3f3f46
search_bg = #27272a
text = #fafafa
text_dim = #a1a1aa
text_faint = #71717a
"""
if open(ini).read() != content:
    open(ini, "w").write(content)
    print("[+] theme.ini Skift palette")

# 11. wallpaper
src_wp = "/tmp/hideo/src/hideo-shell/res/wallpapers/abstract.png"
dst_wp = os.path.join(ROOT, "kora/wallpapers/skift_abstract.png")
if os.path.exists(src_wp) and not os.path.exists(dst_wp):
    subprocess.run(["python3", "-c",
        "from PIL import Image; Image.open('%s').convert('RGB').save('%s')" % (src_wp, dst_wp)])
    print("[+] wallpaper skift_abstract.png")
elif os.path.exists(dst_wp):
    print("[ok] wallpaper skift_abstract.png")
else:
    print("[??] wallpaper source missing (/tmp/hideo...)")

# 12. SYS_AUDIO_VOL + SYS_AUTH_VERIFY (real volume + real lock auth)
patch("kernel/include/yart/syscall.h",
      "    SYS_TASK_LIST   = 79, /* List tasks pids (ps)                                        */\n    SYS_FB_PRESENT  = 80, /* WM: copy only the given damaged rects to the scanout        */\n    SYS_MAX",
      "    SYS_TASK_LIST   = 79, /* List tasks pids (ps)                                        */\n    SYS_FB_PRESENT  = 80, /* WM: copy only the given damaged rects to the scanout        */\n    SYS_AUDIO_VOL   = 81, /* set (a0>=0) / get (a0<0) output volume 0..100               */\n    SYS_AUTH_VERIFY = 82, /* check a password without elevating (session unlock)         */\n    SYS_MAX",
      "syscall.h AUDIO_VOL + AUTH_VERIFY")

patch("kernel/include/yart/audio.h",
      "u64  audio_stream_position(void);   /* bytes played by the output stream  */",
      "u64  audio_stream_position(void);   /* bytes played by the output stream  */\nvoid audio_set_volume(int v);       /* 0..100, drives the DAC amp gain    */\nint  audio_get_volume(void);        /* last value applied to the amp      */",
      "audio.h volume API")

_hda = os.path.join(ROOT, "kernel/drivers/hda.c")
_hdas = open(_hda).read()
if "audio_set_volume" not in _hdas:
    _hdas = _hdas.replace(
      "u64  audio_stream_position(void) { return g_played; }",
      "u64  audio_stream_position(void) { return g_played; }\n\n"
      "/* Real output-volume control: drives the codec's output-amp gain verb. */\n"
      "static int g_hda_vol = 100;\n"
      "void audio_set_volume(int v) {\n"
      "    if (v < 0) v = 0;\n    if (v > 100) v = 100;\n    g_hda_vol = v;\n    if (!g_up) return;\n"
      "    u16 gain = (u16)(((100 - v) * 80) / 100);\n"
      "    u16 payload = gain & 0x7F;\n    if (v == 0) payload |= 0x80;\n"
      "    verb(g_dac_node, V_SET_AMP_GAIN_MUTE, payload); verb_resp();\n"
      "    verb(g_pin_node, V_SET_AMP_GAIN_MUTE, payload); verb_resp();\n"
      "}\nint  audio_get_volume(void) { return g_hda_vol; }\n", 1)
    open(_hda, "w").write(_hdas)
    print("[+] hda.c volume control")
else:
    print("[ok] hda.c volume control")

patch("kernel/arch/x86_64/syscall.c",
      "#include <yart/gui.h>\n#include <yart/drivers.h>",
      "#include <yart/gui.h>\n#include <yart/audio.h>\n#include <yart/drivers.h>",
      "syscall.c include audio.h")

patch("kernel/arch/x86_64/syscall.c",
      "static i64 sys_task_list(u32 *pids, u64 max);\nstatic void sys_sigreturn(cpu_regs_t *r);",
      "static i64 sys_task_list(u32 *pids, u64 max);\nstatic i64 sys_audio_vol(int v);\nstatic i64 sys_auth_verify(const char *password);\nstatic void sys_sigreturn(cpu_regs_t *r);",
      "syscall.c forward decls")


# 13. Roblox 2013 cursors (white arrow = cursor, dark arrow = pointer)
#     The environment occasionally wipes kora/cursors/; restore from /uploads.
_up = "/home/user/uploads"
_rb = os.path.join(ROOT, "kora/cursors/roblox-arrow.png")
_rp = os.path.join(ROOT, "kora/cursors/roblox-hand.png")
if os.path.exists(os.path.join(_up, "Roblox 2013--cursor--SweezyCursors.png")):
    import shutil as _sh
    if not os.path.exists(_rb):
        _sh.copyfile(os.path.join(_up, "Roblox 2013--cursor--SweezyCursors.png"), _rb)
        print("[+] roblox cursor restored")
    if not os.path.exists(_rp):
        _sh.copyfile(os.path.join(_up, "Roblox 2013--pointer--SweezyCursors.png"), _rp)
        print("[+] roblox pointer restored")
    if os.path.exists(_rb) and os.path.exists(_rp):
        print("[ok] roblox cursors present")
else:
    print("[??] /home/user/uploads missing (roblox cursors)")

# 14. kernel substance syscalls (audio/auth/notify/battery)
import subprocess as _sp
_sp.run(["python3", os.path.join(ROOT, "scripts", "ensure_kernel.py")])

print("reapply complete — run: make -j iso")
