/* Yart OS - ACPI discovery.
 *
 * 1. Locate the RSDP via Limine and walk the XSDT/RSDT.
 * 2. Find the MADT ("APIC") table and extract the LAPIC address, IOAPIC
 *    address/GSI base and ISA interrupt source overrides for the APIC
 *    subsystem (apic.c).  Everything else is informational for now.
 */
#include <yart/types.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/mm.h>
#include <yart/acpi.h>

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

madt_info_t g_madt;

/* ---------- MADT ---------- */
typedef struct PACKED {
    sdt_hdr_t hdr;
    u32       lapic_addr;
    u32       flags;
} madt_hdr_t;

typedef struct PACKED { u8 type; u8 len; } madt_ent_t;

typedef struct PACKED { u8 uid; u8 apic_id; u32 flags; } madt_lapic_t;
typedef struct PACKED { u8 id; u8 rsv; u32 addr; u32 gsi_base; } madt_ioapic_t;
typedef struct PACKED { u8 bus; u8 source; u32 gsi; u16 flags; } madt_iso_t_full;

static void acpi_parse_madt(sdt_hdr_t *h) {
    madt_hdr_t *m = (madt_hdr_t *)h;
    g_madt.present = true;
    g_madt.lapic_addr = m->lapic_addr;
    kprintf("acpi: MADT lapic_addr=0x%x flags=0x%x\n", m->lapic_addr, m->flags);

    u8 *p = (u8 *)(m + 1);
    u8 *end = (u8 *)h + h->length;
    while (p + 2 <= end) {
        madt_ent_t *e = (madt_ent_t *)p;
        if (e->len < 2 || p + e->len > end) break;
        switch (e->type) {
        case 0: {   /* processor local APIC */
            madt_lapic_t *la = (madt_lapic_t *)(p + 2);
            kprintf("acpi:   LAPIC uid=%u apic_id=%u flags=0x%x\n",
                    la->uid, la->apic_id, la->flags);
            if (g_madt.lapic_count < 16) {
                g_madt.lapics[g_madt.lapic_count].apic_id = la->apic_id;
                g_madt.lapics[g_madt.lapic_count].uid = la->uid;
                g_madt.lapic_count++;
            }
            break;
        }
        case 1: {   /* IOAPIC */
            madt_ioapic_t *io = (madt_ioapic_t *)(p + 2);
            g_madt.ioapic_addr = io->addr;
            g_madt.ioapic_gsi_base = io->gsi_base;
            kprintf("acpi:   IOAPIC id=%u addr=0x%x gsi_base=%u\n",
                    io->id, io->addr, io->gsi_base);
            break;
        }
        case 2: {   /* interrupt source override */
            madt_iso_t_full *iso = (madt_iso_t_full *)(p + 2);
            if (g_madt.override_count < 16) {
                g_madt.override[g_madt.override_count].irq   = iso->source;
                g_madt.override[g_madt.override_count].gsi   = iso->gsi;
                g_madt.override[g_madt.override_count].flags = iso->flags;
                g_madt.override_count++;
            }
            kprintf("acpi:   ISO irq%u -> gsi%u flags=0x%x\n",
                    iso->source, iso->gsi, iso->flags);
            break;
        }
        default:
            break;
        }
        p += e->len;
    }
}

/* ---------- entry ---------- */
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
    for (int i = 0; i < n; i++) {
        u64 ptr;
        if (entry_size == 8) ptr = ((u64 *)(root + 1))[i];
        else                 ptr = (u64)((u32 *)(root + 1))[i];
        sdt_hdr_t *h = phys_to_virt(ptr);
        char sig[5] = {0};
        memcpy(sig, h->sig, 4);
        kprintf("  [%d] %s len=%u\n", i, sig, h->length);
        if (memcmp(sig, "APIC", 4) == 0)
            acpi_parse_madt(h);
    }
}
