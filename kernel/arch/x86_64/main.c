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
    pic_remap(32, 40);
    /* mask all PIC IRQs initially; drivers will unmask theirs */
    for (int i = 0; i < 16; i++) pic_mask(i);

    /* memory */
    pmm_init(memmap_request.response);
    vmm_init();
    heap_init();

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
    config_load("/etc/yart.conf");

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
    pit_init(100);             /* 100 Hz */
    kbd_init();
    mouse_init();

    /* desktop */
    desktop_init();

    /* go */
    sti();

    /* Bring up /bin/init as a real ring-3 task before entering the
       desktop loop.  init writes to /home/yart/INIT_RAN.txt and exits;
       sys_exit longjmps back here. */
    {
        vnode_t *initbin = vfs_lookup("/bin/init");
        if (initbin) {
            kprintf("yart: launching /bin/init in ring 3...\n");
            user_run_elf(initbin);
        } else {
            kprintf("yart: /bin/init not found, skipping ring-3 launch\n");
        }
    }

    kprintf("yart: kernel up; entering desktop loop.\n");

    /* The longjmp out of sys_exit returns with IF=0 because the CPU
       cleared it on the int 0x80 entry.  Re-enable IRQs before the loop
       or the PIT and PS/2 will never wake hlt. */
    sti();

    /* event loop */
    for (;;) {
        desktop_tick(pit_ticks() * 10);   /* PIT @100Hz -> ms */
        __asm__ volatile ("hlt");
    }
}
