/* Yart OS - tiny ACPI sniffer.
 *
 * Limine hands us the RSDP pointer.  We just dump the OEM ID + revision and
 * count the SDT entries to prove the table is reachable.  This is the seed
 * for HPET / MADT (LAPIC/IOAPIC) discovery later.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>

typedef struct PACKED {
    char     sig[8];        /* "RSD PTR " */
    u8       checksum;
    char     oem_id[6];
    u8       revision;
    u32      rsdt_addr;
    /* ACPI 2.0+ */
    u32      length;
    u64      xsdt_addr;
    u8       ext_checksum;
    u8       reserved[3];
} rsdp_t;

typedef struct PACKED {
    char     sig[4];
    u32      length;
    u8       revision;
    u8       checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    u32      oem_revision;
    u32      creator_id;
    u32      creator_revision;
} sdt_hdr_t;

void acpi_init(void *rsdp_phys_or_virt) {
    if (!rsdp_phys_or_virt) {
        kprintf("acpi: no RSDP supplied\n");
        return;
    }
    rsdp_t *r = rsdp_phys_or_virt;
    char oem[7] = {0};
    memcpy(oem, r->oem_id, 6);
    kprintf("acpi: RSDP rev=%u OEM='%s'\n", r->revision, oem);

    sdt_hdr_t *root;
    int entry_size;
    if (r->revision >= 2 && r->xsdt_addr) {
        root = phys_to_virt(r->xsdt_addr);
        entry_size = 8;
        kprintf("acpi: using XSDT @ %p\n", root);
    } else {
        root = phys_to_virt((paddr_t)r->rsdt_addr);
        entry_size = 4;
        kprintf("acpi: using RSDT @ %p\n", root);
    }
    int n = (root->length - sizeof *root) / entry_size;
    kprintf("acpi: %d SDT entries\n", n);
    for (int i = 0; i < n && i < 16; i++) {
        u64 ptr;
        if (entry_size == 8) {
            ptr = ((u64 *)(root + 1))[i];
        } else {
            ptr = (u64)((u32 *)(root + 1))[i];
        }
        sdt_hdr_t *h = phys_to_virt(ptr);
        char sig[5] = {0};
        memcpy(sig, h->sig, 4);
        kprintf("  [%d] %s len=%u\n", i, sig, h->length);
    }
}
