/* =============================================================================
 *  Yart OS - kernel entry point.
 *
 *  Boot flow:
 *    Limine  -> kmain()  (this file)
 *      console_init  -> serial banner
 *      gdt + idt + pic + pit + rtc
 *      pmm + vmm + heap
 *      framebuffer + desktop
 *      vfs (mount initrd)
 *      keyboard + mouse
 *      irq enable
 *      main loop : desktop_tick(); hlt
 * ===========================================================================*/
#include <yart/types.h>
#include <yart/limine.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/io.h>
#include <yart/hal.h>
#include <yart/mm.h>
#include <yart/gui.h>
#include <yart/fs.h>
#include <yart/drivers.h>
#include <yart/acpi.h>
#include <yart/syscall.h>
#include <yart/config.h>
#include <yart/pci.h>
#include <yart/user.h>
#include <yart/sched.h>
#include <yart/blk.h>
#include <yart/session.h>
#include <yart/cpu.h>    /* fpu_enable() */
int smp_start_aps(void);   /* smp.c */
void smp_ap_kwork_demo(void); /* smp.c */

LIMINE_BASE_REVISION(2);
LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0, .response = 0
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST, .revision = 0, .response = 0
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST, .revision = 0, .response = 0
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request mod_request = {
    .id = LIMINE_MODULE_REQUEST, .revision = 0, .response = 0
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_bootloader_info_request bli_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST, .revision = 0, .response = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST, .revision = 0, .response = 0
};

LIMINE_REQUESTS_END_MARKER;

static void banner(void) {
    serial_puts(
        "\n"
        "================================================================\n"
        "  Y A R T   O S   " YART_VERSION "  -  neon-purple\n"
        "  64-bit  /  Limine  /  framebuffer  /  custom desktop\n"
        "================================================================\n"
    );
    if (bli_request.response) {
        kprintf("boot: %s %s\n",
                bli_request.response->name,
                bli_request.response->version);
    }
}

void kmain(void) {
    serial_init();
    banner();

    if (!fb_request.response || fb_request.response->framebuffer_count < 1)
        kpanic("Limine framebuffer not available");
    if (!memmap_request.response) kpanic("Limine memmap not available");
    if (!hhdm_request.response)   kpanic("Limine HHDM not available");

    g_hhdm_offset = hhdm_request.response->offset;

    /* HAL */
    gdt_init();
    idt_init();
    fpu_enable();                /* x87 + SSE on; clean FPU state         */
    pic_remap(32, 40);
    /* mask all PIC IRQs initially; drivers will unmask theirs */
    for (int i = 0; i < 16; i++) pic_mask(i);

    /* memory */
    pmm_init(memmap_request.response);
    vmm_init();
    heap_init();

    /* memory-subsystem selftests (allocator accounting, refcounts, demand
     * paging, copy-on-write, per-process page tables, heap canaries) */
    pmm_selftest();
    vmm_selftest();
    heap_selftest();

    /* framebuffer + desktop */
    fb_init(fb_request.response->framebuffers[0]);
    kprintf("Welcome to Yart OS\n");

    /* VFS over the first module (initrd.tar) */
    void *initrd = NULL; size_t initrd_size = 0;
    if (mod_request.response && mod_request.response->module_count > 0) {
        initrd      = mod_request.response->modules[0]->address;
        initrd_size = mod_request.response->modules[0]->size;
        kprintf("initrd: %lu KiB at %p\n", initrd_size / 1024, initrd);
    }
    vfs_init(initrd, initrd_size);
    blk_init();          /* find + bring up the virtio-blk disk (if any) */
    blkfs_init();        /* format on first boot, otherwise mount disk   */
    config_load("/etc/yart.conf");   /* disk copy wins over initrd copy  */

    /* ACPI */
    if (rsdp_request.response)
        acpi_init(rsdp_request.response->address);
    else
        kprintf("acpi: no RSDP from bootloader\n");

    /* syscall dispatcher (for future ring-3 tasks) */
    pci_init();
    syscall_install();

    /* Print initrd /etc/motd if present */
    vnode_t *motd = vfs_lookup("/etc/motd");
    if (motd) {
        char buf[512] = {0};
        int n = vfs_read(motd, buf, 0,
                         motd->size < sizeof buf - 1 ? motd->size : sizeof buf - 1);
        if (n > 0) { buf[n] = 0; serial_puts(buf); }
    }

    /* timing + drivers */
    pit_init(100);             /* 100 Hz (APIC timer takes over if available) */
    apic_init();               /* LAPIC/IOAPIC + APIC timer, PIC fallback   */
    smp_start_aps();           /* bring the other cores online (SMP)       */
    kbd_init();
    mouse_init();

    /* desktop */
    desktop_init();

    /* preemptive scheduler: the desktop loop becomes the idle task (pid 0) */
    sched_init();

    /* A root-owned secret file used by the permission/doas boot test. */
    {
        vnode_t *d = vfs_lookup("/etc");
        if (d) {
            vnode_t *v = vfs_lookup("/etc/secret.txt");
            if (!v) v = vfs_create(d, "secret.txt", VN_FILE);
            if (v) {
                v->uid = 0; v->mode = 0600; v->dirty = true;
                vfs_truncate(v, 0);
                const char *data = "TOP-SECRET: 42\n";
                vfs_write(v, data, 0, strlen(data));
            }
        }
    }

    /* Bring up /bin/init as a preemptively-scheduled ring-3 task.  It runs
       alongside the desktop loop; the scheduler time-slices it. */
    vnode_t *initbin = vfs_lookup("/bin/init");
    if (initbin) {
        u64 entry = 0, rsp = 0;
        kprintf("yart: preparing /bin/init for ring 3...\n");
        if (user_prepare_elf(initbin, &entry, &rsp)) {
            task_t *it = sched_create_user("init", entry, rsp);
            if (it) {
                /* assign the logged-in user's identity to the init task */
                u32 uid = 1000;
                const char *acct = "root";
                bool admin = true;
                if (g_session.current_user >= 0) {
                    uid  = 1000u + (u32)g_session.current_user;
                    acct = g_session.users[g_session.current_user].username;
                    admin = g_session.users[g_session.current_user].is_admin;
                }
                it->uid = uid; it->euid = uid; it->gid = uid;
                it->elev_allowed = admin;
                strncpy(it->account, acct, sizeof it->account - 1);
                /* give the user their home directory */
                vnode_t *hy = vfs_lookup("/home/yart");
                if (hy) { hy->uid = uid; hy->mode = 0755; hy->dirty = true; }
                kprintf("yart: init task uid=%u account=%s admin=%d\n",
                        uid, acct, admin ? 1 : 0);
            }
        } else {
            kprintf("yart: /bin/init prepare failed\n");
        }
    } else {
        kprintf("yart: /bin/init not found, skipping ring-3 launch\n");
    }

    kprintf("yart: kernel up; entering desktop loop (idle task pid 0).\n");
    sti();
    smp_ap_kwork_demo();   /* queue kernel work to every AP (wake-IPI proof) */

    /* event loop - runs as the idle task; sched_idle_sleep() hands the CPU
       to user tasks when they are ready.  User tasks run on the APs too:
       /bin/init forks children and the scheduler load-balances them onto
       the least-loaded core (see the 'smp:' lines for each child's CPU). */
    u64 last_sync = 0;
    for (;;) {
        desktop_tick(pit_ticks() * 10);   /* PIT/APIC @100Hz -> ms */
        sched_reap_orphans();
        /* persist dirty files about once a second (disk-backed) */
        if (blkfs_active() && pit_ticks() - last_sync >= 100) {
            int n = blkfs_sync();
            last_sync = pit_ticks();
            if (n > 0)
                kprintf("blkfs: synced %d file(s) to disk (blk irqs=%u)\n",
                        n, blk_irq_count());
        }
        sched_idle_sleep();
    }
}
