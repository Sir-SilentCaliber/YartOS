/* =============================================================================
 *  Yart OS - kernel entry point.
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
#include <yart/pci.h>
#include <yart/user.h>
#include <yart/sched.h>
#include <yart/blk.h>
#include <yart/cpu.h>
#include <yart/watchdog.h>
#include <yart/net.h>
#include <yart/audio.h>
#include <yart/usb.h>
int smp_start_aps(void);
void smp_ap_kwork_demo(void);

/* Watchdog index for the ring-3 compositor.  The compositor kicks the
 * watchdog on every SYS_FB_FLIP; if frames stop arriving the supervisor
 * invokes wm_reset() which paints a recovery screen. */
int g_desktop_wd;

static void wm_reset(void) {
    kprintf("wm: watchdog recovery - painting recovery screen\n");
    fb_clear(0xFF301010);
    fb_present();
}

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
        "  Y A R T   O S   " YART_VERSION "  -  ring-3 compositor\n"
        "  64-bit  /  Limine  /  framebuffer  /  userspace desktop\n"
        "================================================================\n"
    );
    if (bli_request.response)
        kprintf("boot: %s %s\n", bli_request.response->name, bli_request.response->version);
}

void kmain(void) {
    serial_init();
    banner();

    if (!fb_request.response || fb_request.response->framebuffer_count < 1)
        kpanic("Limine framebuffer not available");
    if (!memmap_request.response) kpanic("Limine memmap not available");
    if (!hhdm_request.response)   kpanic("Limine HHDM not available");
    g_hhdm_offset = hhdm_request.response->offset;

    gdt_init();
    idt_init();
    fpu_enable();
    pic_remap(32, 40);
    for (int i = 0; i < 16; i++) pic_mask(i);

    pmm_init(memmap_request.response);

    /* Reserve the initrd module's physical frames NOW, before anything
     * allocates: Limine loads modules into memory the memmap still marks
     * USABLE, so without this the PMM would hand those frames out and
     * ZERO them - corrupting the initrd mid-import. */
    if (mod_request.response && mod_request.response->module_count > 0) {
        paddr_t mod_phys = (paddr_t)((u64)mod_request.response->modules[0]->address -
                                     g_hhdm_offset);
        size_t mod_pages = PAGE_ALIGN_UP(
            mod_request.response->modules[0]->size) / PAGE_SIZE;
        pmm_mark_range_used(mod_phys, mod_pages);
        kprintf("initrd: reserving %lu KiB @ phys %p\n",
                mod_request.response->modules[0]->size / 1024,
                (void *)mod_phys);
    }

    vmm_init();
    heap_init();

    pmm_selftest();
    vmm_selftest();
    heap_selftest();

    fb_init(fb_request.response->framebuffers[0]);
    kprintf("Welcome to Yart OS\n");

    void *initrd = NULL; size_t initrd_size = 0;
    if (mod_request.response && mod_request.response->module_count > 0) {
        initrd = mod_request.response->modules[0]->address;
        initrd_size = mod_request.response->modules[0]->size;
        kprintf("initrd: %lu KiB at %p\n", initrd_size / 1024, initrd);
    }
    vfs_init(initrd, initrd_size);
    blk_init();
    blkfs_init();
    vmm_swap_disk_init();

    if (rsdp_request.response) acpi_init(rsdp_request.response->address);
    else kprintf("acpi: no RSDP from bootloader\n");

    pci_init();
    syscall_install();
    doas_init();

    vnode_t *motd = vfs_lookup("/etc/motd");
    if (motd) {
        char buf[512] = {0};
        int n = vfs_read(motd, buf, 0,
            motd->size < sizeof buf - 1 ? motd->size : sizeof buf - 1);
        if (n > 0) { buf[n] = 0; serial_puts(buf); }
    }

    pit_init(100);
    apic_init();
    smp_start_aps();
    /* APs are up: Limine's SMP trampoline is no longer needed, so the
     * whole direct map (heap/stacks/fb/initrd/DMA buffers) can be NX. */
    vmm_nx_direct_map();
    kbd_init();
    mouse_init();

    sched_init();
    oom_selftest();

    nic_init();
    net_init();
    audio_init();
    usb_init();

    /* /etc/secret.txt used by the doas/perm boot test */
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
    /* user home */
    vfs_mkdir_p("/home/yart");
    blkfs_selftest();      /* 64 KiB through indirect blocks + CRCs */

    /* Load /bin/init as the ring-3 compositor (wm). */
    vnode_t *initbin = vfs_lookup("/bin/init");
    if (initbin) {
        u64 entry = 0, rsp = 0;
        u64 *pml4 = NULL;
        user_region_t regions[MAX_USER_REGIONS];
        int nregions = 0;
        kprintf("yart: loading ring-3 compositor (/bin/init)...\n");
        if (user_prepare_elf(initbin, &entry, &rsp, &pml4, regions, &nregions)) {
            task_t *it = sched_create_user("wm", entry, rsp, pml4, regions, nregions);
            if (it) {
                it->uid = it->euid = it->gid = 1000;
                it->elev_allowed = true;
                strncpy(it->account, "demo", sizeof it->account - 1);
                vnode_t *hy = vfs_lookup("/home/yart");
                if (hy) { hy->uid = 1000; hy->mode = 0755; hy->dirty = true; }
                kprintf("yart: wm task pid=%u (ring-3 compositor)\n", it->pid);
            }
        } else kprintf("yart: /bin/init ELF prepare failed\n");
    } else kprintf("yart: /bin/init not found\n");

    watchdog_selftest();
    g_desktop_wd = watchdog_register_service("wm", wm_reset);

    kprintf("yart: kernel up.  compositor owns the screen.\n");
    sti();
    smp_ap_kwork_demo();

    u64 last_sync = 0;
    for (;;) {
        watchdog_kick(g_desktop_wd);
        net_service();
        audio_poll();
        usb_hid_poll();
        sched_reap_orphans();
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
