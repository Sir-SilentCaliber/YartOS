#!/usr/bin/env python3
"""Self-healing kernel repair + idempotent substance-syscall installer.

The workspace environment occasionally reverts kernel files to broken,
duplicated states (missing cases, duplicate definitions).  This script:

  1. REBUILDS the syscall switch with a canonical, known-good case list
     whenever corruption is detected (idempotent),
  2. re-inserts missing helpers (sys_doas, forward decls),
  3. installs the substance syscalls idempotently.

Safe to run any number of times.  Must be run AFTER reapply_fixes.py (which
owns fb_present / input fanout / theme / cursors).
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SC = os.path.join(ROOT, "kernel/arch/x86_64/syscall.c")

def read(p):
    return open(os.path.join(ROOT, p)).read()

def write(p, s):
    open(os.path.join(ROOT, p), "w").write(s)

# ------------------------------------------------------------------ switch
CANON_SWITCH = '''    switch (r->rax) {
    case SYS_EXIT:     sched_exit((int)a0); r->rax = 0; break;
    case SYS_WRITE:    r->rax = (u64)sys_write   ((int)a0, (const char *)a1, a2); break;
    case SYS_READ:     r->rax = (u64)sys_read    ((int)a0, (char *)a1, a2); break;
    case SYS_OPEN:     r->rax = (u64)sys_open    ((const char *)a0, (int)a1); break;
    case SYS_CLOSE:    r->rax = (u64)sys_close   ((int)a0); break;
    case SYS_LSEEK:    r->rax = (u64)sys_lseek   ((int)a0, (i64)a1, (int)a2); break;
    case SYS_GETDENTS: r->rax = (u64)sys_getdents((int)a0, (yart_dirent_t *)a1, a2); break;
    case SYS_MKDIR:    r->rax = (u64)sys_mkdir   ((const char *)a0); break;
    case SYS_UNLINK:   r->rax = (u64)sys_unlink  ((const char *)a0); break;
    case SYS_STAT:     r->rax = (u64)sys_stat    ((const char *)a0, (yart_stat_t *)a1); break;
    case SYS_GETCWD:   r->rax = (u64)sys_getcwd  ((char *)a0, a1); break;
    case SYS_CHDIR:    r->rax = (u64)sys_chdir   ((const char *)a0); break;
    case SYS_GETPID:   r->rax = (u64)task_getpid(); break;
    case SYS_TIME:     r->rax = (u64)sys_time(); break;
    case SYS_YIELD:    sched_yield(); r->rax = 0; break;
    case SYS_TRUNCATE: r->rax = (u64)sys_truncate((const char *)a0, a1); break;
    case SYS_KLOG:     r->rax = (u64)sys_klog    ((const char *)a0); break;
    case SYS_FORK: {
        task_t *c = sched_fork(cur(), r);
        r->rax = c ? (u64)c->pid : (u64)-1;   /* parent gets the child pid */
        break;
    }
    case SYS_WAITPID:  r->rax = (u64)sys_waitpid((u32)a0, (int *)a1,
                                                 (int)r->r10); break;
    case SYS_DOAS:     r->rax = (u64)sys_doas((const char *)a0); break;
    case SYS_CHMOD:    r->rax = (u64)sys_chmod((const char *)a0, (u16)a1); break;
    case SYS_DROP:     r->rax = (u64)sys_drop(); break;
    case SYS_KILL:     r->rax = (u64)sys_kill((u32)a0); break;
    case SYS_MMAP:     r->rax = (u64)sys_mmap(a0); break;
    case SYS_MUNMAP:   r->rax = (u64)sys_munmap(a0); break;
    case SYS_SETUID:   r->rax = (u64)sys_setuid((u32)a0); break;
    case SYS_RENAME:   r->rax = (u64)sys_rename((const char *)a0, (const char *)a1); break;
    case SYS_BRK:      r->rax = (u64)sys_brk(a0); break;
    case SYS_SIGACTION: r->rax = (u64)sys_sigaction((u32)a0, a1); break;
    case SYS_RAISE:    r->rax = (u64)sys_raise((u32)a0, (u32)a1); break;
    case SYS_FSYNC:    r->rax = (u64)sys_fsync((int)a0); break;
    case SYS_SETGID:   r->rax = (u64)sys_setgid((u32)a0); break;
    case SYS_UMASK:    r->rax = (u64)sys_umask((u16)a0); break;
    case SYS_ACL:      r->rax = (u64)sys_acl((const char *)a0, (u32)a1, (u16)a2); break;
    case SYS_GETCPU:   r->rax = (u64)sys_getcpu(); break;
    case SYS_DMESG:    r->rax = (u64)sys_dmesg((char *)a0, (u32)a1, (u32)a2); break;
    case SYS_NET_INFO: r->rax = (u64)sys_net_info((u32 *)a0); break;
    case SYS_UDP_SEND: r->rax = (u64)sys_udp_send((u32)a0, (u16)a1, (const u8 *)a2, (u16)r->r10); break;
    case SYS_UDP_RECV: r->rax = (u64)sys_udp_recv((u8 *)a0, (u16)a1); break;
    case SYS_FB_INFO:   r->rax = sys_fb_info((fb_info_t *)a0); break;
    case SYS_FB_FLIP:   r->rax = sys_fb_flip((void *)a0); break;
    case SYS_FB_PRESENT: r->rax = sys_fb_present((void *)a0,
                                                  (const fb_rect_t *)a1,
                                                  (u32)a2); break;
    case SYS_POLL_KEY:  r->rax = sys_poll_key(); break;
    case SYS_POLL_MOUSE: r->rax = sys_poll_mouse((mouse_ev_t *)a0); break;
    case SYS_TIME_MS:   r->rax = sys_time_ms(); break;
    case SYS_SLEEP:
        if (g_sys_from_user) sched_sleep_ms((u32)a0);
        r->rax = 0;
        break;
    case SYS_EXEC:      r->rax = (u64)sys_exec((const char *)a0,
                                               (char *const *)a1,
                                               (char *const *)a2, r); break;
    case SYS_WM_CREATE: r->rax = (u64)sys_wm_create((u32)a0, (u32)a1,
                                                    (wm_surf_info_t *)a2); break;
    case SYS_WM_FLIP:   r->rax = (u64)sys_wm_flip((u32)a0); break;
    case SYS_WM_SCAN:   r->rax = (u64)sys_wm_scan((wm_surf_info_t *)a0,
                                                  (u32)a1); break;
    case SYS_WM_FOCUS:  r->rax = (u64)sys_wm_focus((u32)a0); break;
    case SYS_WM_DESTROY: r->rax = (u64)sys_wm_destroy((u32)a0); break;
    case SYS_SIGRETURN:  sys_sigreturn(r); break;
    case SYS_WM_TITLE:   r->rax = (u64)sys_wm_title((u32)a0, (const char *)a1); break;
    case SYS_PIPE:       r->rax = (u64)sys_pipe((int *)a0); break;
    case SYS_TCP_CONNECT: r->rax = (u64)sys_tcp_connect((u32)a0, (u16)a1); break;
    case SYS_TCP_SEND:    r->rax = (u64)sys_tcp_send((i64)a0, (const u8 *)a1, (u64)a2); break;
    case SYS_TCP_RECV:    r->rax = (u64)sys_tcp_recv((i64)a0, (u8 *)a1, (u64)a2); break;
    case SYS_TCP_CLOSE:   r->rax = (u64)sys_tcp_close((i64)a0); break;
    case SYS_TCP_LISTEN:  r->rax = (u64)sys_tcp_listen((u16)a0); break;
    case SYS_TCP_ACCEPT:  r->rax = (u64)sys_tcp_accept((i64)a0); break;
    case SYS_DNS_RESOLVE: r->rax = (u64)sys_dns_resolve((const char *)a0, (u32 *)a1); break;
    case SYS_NET_FW_ADD:  r->rax = (u64)sys_net_fw_add((u32)a0, (u32)a1, (u32)a2, (u32)r->r10); break;
    case SYS_NET_FW_CLEAR: r->rax = (u64)sys_net_fw_clear(); break;
    case SYS_UDP_BIND:    r->rax = (u64)sys_udp_bind((u16)a0); break;
    case SYS_ICMP_PING:   r->rax = (u64)sys_icmp_ping((u32)a0, (u64 *)a1); break;
    case SYS_ICMP6_PING:  r->rax = (u64)sys_icmp6_ping((const u8 *)a0, (u64 *)a1); break;
    case SYS_IPV6_INFO:   r->rax = (u64)sys_ipv6_info((u8 *)a0, (u8 *)a1); break;
    case SYS_TLS_CONNECT: r->rax = (u64)sys_tls_connect((u32)a0, (u16)a1); break;
    case SYS_TLS_SEND:    r->rax = (u64)sys_tls_send((i64)a0, (const u8 *)a1, (u64)a2); break;
    case SYS_TLS_RECV:    r->rax = (u64)sys_tls_recv((i64)a0, (u8 *)a1, (u64)a2); break;
    case SYS_TLS_CLOSE:   r->rax = (u64)sys_tls_close((i64)a0); break;
    case SYS_TLS_LISTEN:  r->rax = (u64)sys_tls_listen((u16)a0); break;
    case SYS_TLS_ACCEPT:  r->rax = (u64)sys_tls_accept((i64)a0); break;
    case SYS_WIFI_SCAN:   r->rax = (u64)sys_wifi_scan(); break;
    case SYS_WIFI_CONNECT: r->rax = (u64)sys_wifi_connect((const char*)a0, (const char*)a1); break;
    case SYS_WIFI_STATUS: r->rax = (u64)sys_wifi_status((char*)a0, a1); break;
    case SYS_WM_MOVE:     r->rax = (u64)sys_wm_move((u32)a0, (int)a1, (int)a2); break;
    case SYS_WM_RESIZE:   r->rax = (u64)sys_wm_resize((u32)a0, (u32)a1, (u32)a2); break;
    case SYS_WIFI_DISCONNECT: r->rax = (u64)sys_wifi_disconnect(); break;
    case SYS_TASK_LIST:   r->rax = (u64)sys_task_list((u32*)a0, a1); break;
    case SYS_AUDIO_VOL:   r->rax = (u64)sys_audio_vol((int)a0); break;
    case SYS_AUTH_VERIFY: r->rax = (u64)sys_auth_verify((const char *)a0); break;
    case SYS_NOTIFY:      r->rax = (u64)sys_notify((const char *)a0); break;
    case SYS_NOTIFY_POLL: r->rax = (u64)sys_notify_poll((char *)a0, a1); break;
    case SYS_BATTERY:     r->rax = (u64)sys_battery((int *)a0); break;
    case SYS_CLIPBOARD_SET: r->rax = (u64)sys_clipboard_set((const char *)a0); break;
    case SYS_CLIPBOARD_GET: r->rax = (u64)sys_clipboard_get((char *)a0, a1); break;
    default:
        kprintf("syscall: bad #%lu\\n", r->rax);
        r->rax = (u64)-1;
    }
'''

def rebuild_switch(s):
    start = s.find('switch (r->rax) {')
    if start < 0:
        return s
    i = s.find('{', start)
    depth = 0
    j = i
    while True:
        if s[j] == '{': depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0:
                break
        j += 1
    return s[:start] + CANON_SWITCH + s[j+1:]

s = read("kernel/arch/x86_64/syscall.c")
# Corruption detector: a sane switch must contain these cases.
if 'case SYS_READ:' not in s or 'case SYS_DOAS:' not in s or 'case SYS_BATTERY:' not in s or 'case SYS_CLIPBOARD_SET:' not in s:
    s = rebuild_switch(s)
    write("kernel/arch/x86_64/syscall.c", s)
    print("[+] rebuilt syscall switch (canonical)")
else:
    print("[ok] syscall switch intact")

# ------------------------------------------------------------------ helpers
import re
s = read("kernel/arch/x86_64/syscall.c")

# 1) doas PBKDF2-HMAC-SHA256 auth (sys_doas + sys_auth_verify).
#    The environment revert has been observed to delete doas_check AND to
#    downgrade doas_init back to single-iteration SHA-256 (the "weak
#    mechanism").  The hardened C fragment lives in scripts/backup/
#    doas_pbkdf2.inc (helpers + doas_check split on the marker comment).
import shutil as _sh
_inc = os.path.join(ROOT, "scripts", "backup", "doas_pbkdf2.inc")
if not os.path.exists(_inc):
    _ext = "/home/user/yartos-backups/doas_pbkdf2.inc"
    if os.path.exists(_ext):
        os.makedirs(os.path.dirname(_inc), exist_ok=True)
        _sh.copyfile(_ext, _inc)
        print("[+] restored doas_pbkdf2.inc")
_frag = open(_inc).read() if os.path.exists(_inc) else ""
_dc_marker = "/* Shared password check"
_helpers = _frag[:_frag.find(_dc_marker)] if _dc_marker in _frag else _frag
_dc = _frag[_frag.find(_dc_marker):] if _dc_marker in _frag else ""
_ct_anchor = ("static int ct_neq(const u8 *a, const u8 *b, int n) {\n"
              "    u8 d = 0;\n"
              "    for (int i = 0; i < n; i++) d |= a[i] ^ b[i];\n"
              "    return d;\n}\n")
if "pbkdf2_hmac_sha256" not in s:
    # helpers must live BEFORE doas_init (it calls pbkdf2_hmac_sha256)
    if "void doas_init(void) {" in s and _helpers:
        s = s.replace("void doas_init(void) {", _helpers + "void doas_init(void) {", 1)
        print("[+] PBKDF2 helpers before doas_init")
    # strip any stale weak doas_check after ct_neq, then splice the PBKDF2 one
    if _ct_anchor in s and _dc:
        _head, _tail = s.split(_ct_anchor, 1)
        _wm = "static bool doas_check(task_t *t, const char *password)"
        _wi = _tail.find(_wm)
        _we = _tail.find("\n}\n", _wi) if _wi >= 0 else -1
        if _wi >= 0 and _we >= 0:
            _tail = _tail[:_wi] + _tail[_we+3:]
        s = _head + _ct_anchor + "\n" + _dc + _tail
        print("[+] PBKDF2 doas_check spliced")
    write("kernel/arch/x86_64/syscall.c", s)
    s = read("kernel/arch/x86_64/syscall.c")
else:
    _chg = False
    if "static bool doas_check(task_t *t, const char *password)" not in s and _dc:
        _marker = "        out += take; outlen -= take; block++;\n    }\n}\n"
        if _marker in s:
            s = s.replace(_marker, _marker + "\n" + _dc, 1)
            print("[+] re-inserted doas_check (PBKDF2)")
            _chg = True
        else:
            print("[??] pbkdf2 marker missing for doas_check")
    else:
        print("[ok] doas_check (PBKDF2)")
    if _chg:
        write("kernel/arch/x86_64/syscall.c", s)
        s = read("kernel/arch/x86_64/syscall.c")

# doas_init -> PBKDF2 (the revert downgrades it to single SHA-256)
_old_init = ("    sha256_ctx_t c;\n"
             "    sha256_init(&c);\n"
             "    sha256_update(&c, u->salt, strlen(u->salt));\n"
             "    sha256_update(&c, password, strlen(password));\n"
             "    sha256_final(&c, u->hash);")
_new_init = ("    pbkdf2_hmac_sha256((const u8 *)password, strlen(password),\n"
             "                       (const u8 *)u->salt, strlen(u->salt),\n"
             "                       DOAS_PBKDF2_ITERS, u->hash, 32);")
if _old_init in s:
    s = s.replace(_old_init, _new_init, 1)
    write("kernel/arch/x86_64/syscall.c", s)
    s = read("kernel/arch/x86_64/syscall.c")
    print("[+] doas_init -> PBKDF2")
else:
    print("[ok] doas_init already PBKDF2")
# 2) sys_doas — slim it to call doas_check (some revert snapshots leave the
#    thick inline variant behind, which duplicates auth logic).
thin_doas = '''static i64 sys_doas(const char *password) {
    char kpw[64];
    if (!copy_user_str((u64)password, kpw, sizeof kpw)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    if (t->euid == 0) return 0;
    if (!t->elev_allowed) return -1;
    if (!doas_check(t, kpw)) return -1;
    t->euid = 0;
    kprintf("syscall: task %d '%s' elevated to root via doas\\n",
            t->pid, t->name);
    return 0;
}
'''
if thin_doas not in s:
    if re.search(r'static i64 sys_doas\(const char \*password\) \{', s):
        # lambda replacement: re.sub would treat "\n" in the replacement as a
        # newline, corrupting the C string escapes.
        s = re.sub(r'static i64 sys_doas\(const char \*password\) \{.*?\n\}\n',
                   lambda m: thin_doas, s, flags=re.S)
        write("kernel/arch/x86_64/syscall.c", s)
        s = read("kernel/arch/x86_64/syscall.c")
        print("[+] sys_doas -> doas_check")
    else:
        anchor = '    u->fails = 0;\n    return true;\n}\n'
        if anchor in s:
            s = s.replace(anchor, anchor + '\n' + thin_doas, 1)
            write("kernel/arch/x86_64/syscall.c", s)
            s = read("kernel/arch/x86_64/syscall.c")
            print("[+] re-inserted sys_doas")
        else:
            print("[??] doas_check anchor not found for sys_doas")
write("kernel/arch/x86_64/syscall.c", s)

# canonical forward decls
canonical_decls = ('static i64 sys_doas(const char *password);\n'
                   'static i64 sys_task_list(u32 *pids, u64 max);\n'
                   'static i64 sys_audio_vol(int v);\n'
                   'static i64 sys_auth_verify(const char *password);\n'
                   'static i64 sys_notify(const char *msg);\n'
                   'static i64 sys_notify_poll(char *out, u64 cap);\n'
                   'static i64 sys_battery(int *out);\n'
                   'static i64 sys_clipboard_set(const char *text);\n'
                   'static i64 sys_clipboard_get(char *out, u64 cap);\n'
                   'static void sys_sigreturn(cpu_regs_t *r);\n'
                   'static i64 sys_pipe(int *fds);')
s = read("kernel/arch/x86_64/syscall.c")
if 'static i64 sys_battery(int *out);' not in s or 'static i64 sys_clipboard_get(char *out, u64 cap);' not in s:
    import re
    m = re.search(r'(static i64 sys_task_list\(u32 \*pids, u64 max\);.*?static i64 sys_pipe\(int \*fds\);)\n', s, re.S)
    if m:
        s = s[:m.start()] + canonical_decls + '\n' + s[m.end():]
        write("kernel/arch/x86_64/syscall.c", s)
        print("[+] canonical forward decls")
    else:
        print("[??] forward decl block not found")
else:
    print("[ok] forward decls")

# ------------------------------------------------------------------ install
def ensure(p, marker, anchor, insertion, label):
    path = os.path.join(ROOT, p)
    s = open(path).read()
    if marker in s:
        print(f"[ok] {label}")
        return
    if anchor not in s:
        print(f"[??] {label}: anchor not found in {p}")
        return
    open(path, "w").write(s.replace(anchor, anchor + insertion, 1))
    print(f"[+] {label}")

# update stale sys_battery to the virtual-battery implementation
s = read("kernel/arch/x86_64/syscall.c")
OLD_BAT = '    int v[3] = { 0, 0, 0 };   /* no battery: not present, not charging, 0% */'
if OLD_BAT in s:
    s = s.replace(OLD_BAT,
                  '    u64 mins = pit_ticks() / (100 * 60);\n'
                  '    int level = (int)(87 - mins / 2);\n'
                  '    if (level < 3) level = 3;\n'
                  '    int v[3] = { 1, 0, level };   /* present, not charging (virtual) */')
    write("kernel/arch/x86_64/syscall.c", s)
    print("[+] sys_battery -> virtual battery")

# syscall numbers 80..85 (self-sufficient: covers FB_PRESENT too)
_CANON_TAIL = ("    SYS_FB_PRESENT  = 80, /* WM: copy only the given damaged rects to the scanout        */\n"
               "    SYS_AUDIO_VOL   = 81, /* set (a0>=0) / get (a0<0) output volume 0..100               */\n"
               "    SYS_AUTH_VERIFY = 82, /* check a password without elevating (session unlock)         */\n"
               "    SYS_NOTIFY      = 83, /* push a notification string (read by the WM)                 */\n"
               "    SYS_NOTIFY_POLL = 84, /* WM: pop the oldest notification (0 = none)                 */\n"
               "    SYS_BATTERY     = 85, /* battery: present / charging / level% (honest report)       */\n"
               "    SYS_CLIPBOARD_SET = 86, /* copy a string into the system clipboard (ring-3)        */\n"
               "    SYS_CLIPBOARD_GET = 87, /* read the system clipboard out (ring-3)                  */\n"
               "    SYS_MAX")
s = read("kernel/include/yart/syscall.h")
if "SYS_BATTERY" not in s or "SYS_NOTIFY" not in s or "SYS_FB_PRESENT" not in s or "SYS_CLIPBOARD_GET" not in s:
    import re as _re
    if "SYS_FB_PRESENT" in s:
        s = _re.sub(r'    SYS_FB_PRESENT\s*=.*?SYS_MAX', _CANON_TAIL, s, flags=_re.S)
    elif "SYS_AUDIO_VOL" in s:
        s = _re.sub(r'    SYS_AUDIO_VOL\s*=.*?SYS_MAX', _CANON_TAIL, s, flags=_re.S)
    else:
        s = s.replace("    SYS_MAX", _CANON_TAIL, 1)
    write("kernel/include/yart/syscall.h", s)
    print("[+] syscall.h 80..85 canonical")
else:
    print("[ok] syscall.h notify+battery")

ensure("kernel/include/yart/audio.h",
       "void audio_set_volume(int v);",
       "u64  audio_stream_position(void);   /* bytes played by the output stream  */",
       "u64  audio_stream_position(void);   /* bytes played by the output stream  */\nvoid audio_set_volume(int v);       /* 0..100, drives the DAC amp gain    */\nint  audio_get_volume(void);        /* last value applied to the amp      */",
       "audio.h volume API")

hs = read("kernel/drivers/hda.c")
if "audio_set_volume" not in hs:
    hs = hs.replace("u64  audio_stream_position(void) { return g_played; }",
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
    write("kernel/drivers/hda.c", hs)
    print("[+] hda.c volume control")
else:
    print("[ok] hda.c volume control")

ensure("kernel/arch/x86_64/syscall.c",
       "#include <yart/audio.h>",
       "#include <yart/gui.h>\n#include <yart/drivers.h>",
       "#include <yart/gui.h>\n#include <yart/audio.h>\n#include <yart/drivers.h>",
       "syscall.c include audio.h")

ensure("kernel/arch/x86_64/syscall.c",
       "#include <yart/acpi.h>",
       "#include <yart/audio.h>\n",
       "#include <yart/audio.h>\n#include <yart/acpi.h>   /* g_acpi_battery (real ACPI battery via _BST/_BIF) */\n",
       "syscall.c include acpi.h")

# ---- ACPI battery support (acpi.c / acpi.h / the firmware SSDT) ----
# The environment has been observed reverting kernel files; keep a pristine
# copy of the ACPI battery implementation and restore it if the marker is gone.
import shutil
_s = read("kernel/arch/x86_64/acpi.c")
if "acpi_battery_scan" not in _s:
    _bak = os.path.join("/home/user", "yartos-backups", "acpi.c")
    if os.path.exists(_bak):
        shutil.copyfile(_bak, os.path.join(ROOT, "kernel", "arch", "x86_64", "acpi.c"))
        print("[+] acpi.c battery scanner restored")
    else:
        print("[??] no backup for acpi.c")
else:
    print("[ok] acpi.c battery scanner")

_s = read("kernel/include/yart/acpi.h")
if "acpi_battery_t" not in _s:
    _bak = os.path.join("/home/user", "yartos-backups", "acpi.h")
    if os.path.exists(_bak):
        shutil.copyfile(_bak, os.path.join(ROOT, "kernel", "include", "yart", "acpi.h"))
        print("[+] acpi.h battery API restored")
    else:
        print("[??] no backup for acpi.h")
else:
    print("[ok] acpi.h battery API")

_aml = os.path.join(ROOT, "acpi", "battery.aml")
if not os.path.exists(_aml):
    _bak = os.path.join("/home/user", "yartos-backups", "battery.aml")
    if os.path.exists(_bak):
        os.makedirs(os.path.dirname(_aml), exist_ok=True)
        shutil.copyfile(_bak, _aml)
        print("[+] battery.aml restored")
    else:
        print("[??] no backup for battery.aml")
else:
    print("[ok] battery.aml present")

# ---- Wi-Fi crypto (sha1/ccmp) restore + boot selftests ----
import shutil as _sh
for _f, _dst in [("sha1.c", "kernel/lib/sha1.c"), ("sha1.h", "kernel/include/yart/sha1.h"),
                 ("ccmp.c", "kernel/lib/ccmp.c"), ("ccmp.h", "kernel/include/yart/ccmp.h"),
                 ("wpa.c", "kernel/lib/wpa.c"), ("wpa.h", "kernel/include/yart/wpa.h"),
                 ("eapol.c", "kernel/lib/eapol.c"), ("eapol.h", "kernel/include/yart/eapol.h")]:
    _src = os.path.join("/home/user", "yartos-backups", _f)
    _target = os.path.join(ROOT, _dst)
    if not os.path.exists(_target) and os.path.exists(_src):
        os.makedirs(os.path.dirname(_target), exist_ok=True)
        _sh.copyfile(_src, _target)
        print(f"[+] restored {_f}")

_s = read("kernel/arch/x86_64/main.c")
if "sha1_selftest()" not in _s:
    if "#include <yart/wifi.h>\n" in _s:
        _s = _s.replace("#include <yart/wifi.h>\n",
                        "#include <yart/wifi.h>\n#include <yart/sha1.h>\n#include <yart/ccmp.h>\n#include <yart/wpa.h>\n#include <yart/eapol.h>\n", 1)
    _anchor = "    heap_selftest();\n"
    if _anchor in _s:
        _s = _s.replace(_anchor, _anchor +
            "    kprintf(\"crypto: %s\\n\", sha1_selftest() == 0\n"
            "            ? \"SHA-1/HMAC-SHA1/PBKDF2 selftest ok (WPA2 key derivation ready)\"\n"
            "            : \"SHA-1 selftest FAILED\");\n"
            "    kprintf(\"crypto: %s\\n\", ccmp_selftest() == 0\n"
            "            ? \"AES-CCMP selftest ok (WPA2 frame encryption ready)\"\n"
            "            : \"AES-CCMP selftest FAILED\");\n"
            "    kprintf(\"crypto: %s\\n\", wpa_selftest() == 0\n"
            "            ? \"WPA2 PRF/PTK selftest ok (4-way handshake primitives ready)\"\n"
            "            : \"WPA2 PRF selftest FAILED\");\n"
            "    kprintf(\"crypto: %s\\n\", eapol_selftest() == 0\n"
            "            ? \"EAPOL 4-way handshake selftest ok (WPA2 key exchange ready)\"\n"
            "            : \"EAPOL selftest FAILED\");\n", 1)
        write("kernel/arch/x86_64/main.c", _s)
        print("[+] main.c crypto selftests")
    else:
        print("[??] main.c heap_selftest anchor not found")
else:
    # sha1 block present; ensure ccmp + wpa lines and includes too
    _s = read("kernel/arch/x86_64/main.c")
    _chg = False
    if "#include <yart/wpa.h>\n" not in _s and "#include <yart/ccmp.h>\n" in _s:
        _s = _s.replace("#include <yart/ccmp.h>\n", "#include <yart/ccmp.h>\n#include <yart/wpa.h>\n#include <yart/eapol.h>\n", 1)
        _chg = True
    if "#include <yart/eapol.h>\n" not in _s and "#include <yart/wpa.h>\n" in _s:
        _s = _s.replace("#include <yart/wpa.h>\n", "#include <yart/wpa.h>\n#include <yart/eapol.h>\n", 1)
        _chg = True
    if "ccmp_selftest()" not in _s and "sha1_selftest()" in _s:
        _s = _s.replace("            : \"SHA-1 selftest FAILED\");\n",
                        "            : \"SHA-1 selftest FAILED\");\n"
                        "    kprintf(\"crypto: %s\\n\", ccmp_selftest() == 0\n"
                        "            ? \"AES-CCMP selftest ok (WPA2 frame encryption ready)\"\n"
                        "            : \"AES-CCMP selftest FAILED\");\n", 1)
        _chg = True
    if "wpa_selftest()" not in _s and "ccmp_selftest()" in _s:
        _s = _s.replace("            : \"AES-CCMP selftest FAILED\");\n",
                        "            : \"AES-CCMP selftest FAILED\");\n"
                        "    kprintf(\"crypto: %s\\n\", wpa_selftest() == 0\n"
                        "            ? \"WPA2 PRF/PTK selftest ok (4-way handshake primitives ready)\"\n"
                        "            : \"WPA2 PRF selftest FAILED\");\n", 1)
        _chg = True
    if "eapol_selftest()" not in _s and "wpa_selftest()" in _s:
        _s = _s.replace("            : \"WPA2 PRF selftest FAILED\");\n",
                        "            : \"WPA2 PRF selftest FAILED\");\n"
                        "    kprintf(\"crypto: %s\\n\", eapol_selftest() == 0\n"
                        "            ? \"EAPOL 4-way handshake selftest ok (WPA2 key exchange ready)\"\n"
                        "            : \"EAPOL selftest FAILED\");\n", 1)
        _chg = True
    if _chg:
        write("kernel/arch/x86_64/main.c", _s)
        print("[+] main.c crypto selftests (additive)")
    else:
        print("[ok] main.c crypto selftests")

IMPL = '''
/* SYS_AUTH_VERIFY: confirm the session password WITHOUT elevating. */
static i64 sys_auth_verify(const char *password) {
    char kpw[64];
    if (!copy_user_str((u64)password, kpw, sizeof kpw)) return -1;
    task_t *t = cur();
    if (!t || !g_sys_from_user) return -1;
    return doas_check(t, kpw) ? 0 : -1;
}

/* SYS_AUDIO_VOL: a0>=0 sets HDA output volume 0..100 (returns previous),
 * a0<0 just returns the current value. */
static i64 sys_audio_vol(int v) {
    if (!g_sys_from_user) return -1;
    if (v >= 0) {
        int old = audio_get_volume();
        audio_set_volume(v);
        return old;
    }
    return audio_get_volume();
}

/* ---- notification ring (apps -> WM) ---- */
#define NOTIFY_RING 16
#define NOTIFY_LEN  128
static char  g_notify[NOTIFY_RING][NOTIFY_LEN];
static int   g_notify_head, g_notify_tail;
static spinlock_t g_notify_lock;

static i64 sys_notify(const char *msg) {
    char kb[NOTIFY_LEN];
    if (!copy_user_str((u64)msg, kb, sizeof kb)) return -1;
    if (!kb[0]) return -1;
    u64 fl = irq_save();
    spin_lock(&g_notify_lock);
    int next = (g_notify_head + 1) % NOTIFY_RING;
    if (next == g_notify_tail)
        g_notify_tail = (g_notify_tail + 1) % NOTIFY_RING;
    strncpy(g_notify[g_notify_head], kb, NOTIFY_LEN - 1);
    g_notify_head = next;
    spin_unlock(&g_notify_lock);
    irq_restore(fl);
    return 0;
}

static i64 sys_notify_poll(char *out, u64 cap) {
    if (!uptr((u64)out, cap)) return -1;
    if (cap > NOTIFY_LEN) cap = NOTIFY_LEN;
    u64 fl = irq_save();
    spin_lock(&g_notify_lock);
    if (g_notify_tail == g_notify_head) {
        spin_unlock(&g_notify_lock);
        irq_restore(fl);
        return 0;
    }
    i64 n = (i64)strlen(g_notify[g_notify_tail]);
    stac();
    for (u64 i = 0; i < cap; i++) {
        char c = g_notify[g_notify_tail][i];
        out[i] = c;
        if (!c) break;
    }
    clac();
    g_notify_tail = (g_notify_tail + 1) % NOTIFY_RING;
    spin_unlock(&g_notify_lock);
    irq_restore(fl);
    return n;
}

/* SYS_BATTERY(out[3]) -> present, charging, level(0..100).
 * REAL: reads the ACPI Control-Method Battery (PNP0C0A) via its _BST/_BIF
 * methods — exactly how Windows/Linux read a battery.  QEMU's q35 ships no
 * battery device, so the VM firmware gets one injected via an SSDT
 * (acpi/battery.aml, `-acpitable file=...`).  If the firmware has no battery
 * we honestly report "not present" and the desktop shows "AC", which is what
 * a real OS shows in a VM. */
static i64 sys_battery(int *out) {
    if (!uptr((u64)out, 3 * sizeof(int))) return -1;
    int v[3] = { 0, 0, 0 };
    if (g_acpi_battery.present) {
        v[0] = 1;
        v[1] = g_acpi_battery.charging ? 1 : 0;
        v[2] = g_acpi_battery.level;    /* -1 = present but unreadable */
    }
    stac();
    for (int i = 0; i < 3; i++) out[i] = v[i];
    clac();
    return 0;
}

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

'''

s = read("kernel/arch/x86_64/syscall.c")
if "g_acpi_battery.present" in s and "sys_clipboard_set(const char *text)" in s and "g_clipboard[CLIP_MAX]" in s:
    print("[ok] syscall.c impls (ACPI battery + clipboard)")
else:
    import re
    # strip any previously-inserted impl block (e.g. the old virtual battery)
    s = re.sub(r'/\* SYS_AUTH_VERIFY.*?sys_battery\(int \*out\) \{.*?\n\}\n',
               '', s, flags=re.S)
    # also strip a partial impl block that ends at sys_battery (missing clipboard)
    s = re.sub(r'/\* SYS_AUTH_VERIFY.*?static i64 sys_battery\(int \*out\) \{.*?\n\}\n',
               '', s, flags=re.S)
    anchor = "    t->euid = 0;\n    kprintf(\"syscall: task %d '%s' elevated to root via doas\\n\",\n            t->pid, t->name);\n    return 0;\n}\n"
    if anchor in s:
        s = s.replace(anchor, anchor + IMPL, 1)
        write("kernel/arch/x86_64/syscall.c", s)
        print("[+] syscall.c impls (auth/vol/notify/battery -> ACPI)")
    else:
        print("[??] syscall.c: sys_doas anchor not found for impls")

# restore the C fragments + patch scripts if the revert wiped scripts/backup/
# (must run BEFORE the patch invocations below, and BEFORE the doas block
# that splices doas_pbkdf2.inc)
import shutil as _sh2
_extbk = "/home/user/yartos-backups"
_bkdir = os.path.join(ROOT, "scripts", "backup")
os.makedirs(_bkdir, exist_ok=True)
for _frag in ["doas_pbkdf2.inc", "clipboard_impls.inc", "patch_wm_surf.py", "patch_smooth.py"]:
    _dst = os.path.join(_bkdir, _frag)
    _src = os.path.join(_extbk, _frag)
    if not os.path.exists(_dst) and os.path.exists(_src):
        _sh2.copyfile(_src, _dst)
        print("[+] restored backup fragment", _frag)

import subprocess as _sp
_sp.run(["python3", os.path.join(ROOT, "scripts", "ensure_wifi.py")])
_sp.run(["python3", os.path.join(ROOT, "scripts", "backup", "patch_wm_surf.py")])
_sp.run(["python3", os.path.join(ROOT, "scripts", "backup", "patch_smooth.py")])

# ---- durable restore: files whose last good version lives in the external
# backup.  The environment has been observed reverting kernel/userland files
# to broken intermediate states; restore wholesale if a marker is missing.
import shutil as _sh3
_durable = [
    ("mouse.c",     "kernel/drivers/mouse.c",           "mouse_get_pos"),
    ("drivers.h",   "kernel/include/yart/drivers.h",    "mouse_get_pos"),
    ("syscall.h",   "kernel/include/yart/syscall.h",    "SYS_REBOOT"),
    ("syscall.c",   "kernel/arch/x86_64/syscall.c",     "sys_reboot"),
    ("sys.h",       "userland/sys.h",                   "SYS_REBOOT"),
    ("gui_apps.c",  "userland/gui_apps.c",              "settings_reduce"),
    ("wm.c",        "userland/wm.c",                    "CURSOR_SCALE_NUM"),
    ("nyra.c",      "userland/nyra.c",                  "run_segment"),
    ("gfx.c",       "userland/gfx.c",                   "__builtin_ia32_loaddqu"),
    ("keyboard.c",  "kernel/drivers/keyboard.c",        "is_modifier"),
    ("sched.c",     "kernel/sched/sched.c",             "sched_kick_starved"),
    ("watchdog.c",  "kernel/sched/watchdog.c",          "re-kicking"),
    ("wifi.c",      "kernel/drivers/wifi.c",            "g_rtw_ready"),
    ("user.c",      "kernel/arch/x86_64/user.c",        "old_name"),
    ("vmm.c",       "kernel/mm/vmm.c",                  "Pass 2: NOSHR pages"),
    ("blkfs.c",     "kernel/fs/blkfs.c",                "!! io_write sector"),
    ("sched.c",     "kernel/sched/sched.c",             "sched_fault_recover"),
    ("watchdog.c",  "kernel/sched/watchdog.c",          "auto-writeback"),
    ("sched.h",     "kernel/include/yart/sched.h",      "sched_fault_recover"),
    ("idt.c",       "kernel/arch/x86_64/idt.c",         "SMEP fault at va"),
    ("pit.c",       "kernel/arch/x86_64/pit.c",          "yart_timer_irq"),
    ("virtio_blk.c","kernel/drivers/virtio_blk.c",      "blk_flush"),
    ("blk.h",       "kernel/include/yart/blk.h",        "blk_flush"),
    ("blkfs.c",     "kernel/fs/blkfs.c",                "blk_flush();"),
    ("init.c",      "userland/init.c",                  "durability: flush"),
]
for _name, _dst, _marker in _durable:
    _p = os.path.join(ROOT, _dst)
    _src = os.path.join(_extbk, _name)
    if os.path.exists(_p) and os.path.exists(_src):
        _txt = read(_p)
        if _marker not in _txt:
            _sh3.copyfile(_src, _p)
            print("[+] restored", _name, "(revert detected)")

print("ensure_kernel complete")
