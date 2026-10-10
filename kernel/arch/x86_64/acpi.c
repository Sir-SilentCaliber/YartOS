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

/* DSDT + SSDT AML blobs, kept for the battery/AC namespace scan. */
#define MAX_AML_TABS 16
static struct { u8 *aml; u32 len; } g_aml_tabs[MAX_AML_TABS];
static int g_aml_n;
acpi_battery_t g_acpi_battery;

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
        /* keep DSDT + SSDT AML around for the battery/AC scan */
        if ((memcmp(sig, "DSDT", 4) == 0 || memcmp(sig, "SSDT", 4) == 0)
            && g_aml_n < MAX_AML_TABS && h->length > sizeof(sdt_hdr_t)) {
            g_aml_tabs[g_aml_n].aml = (u8 *)h + sizeof(sdt_hdr_t);
            g_aml_tabs[g_aml_n].len = h->length - sizeof(sdt_hdr_t);
            g_aml_n++;
        }
    }
    acpi_battery_scan();
}

/* ===================== ACPI battery (real, like Windows/Linux) =============
 * A battery is a Control-Method Battery device (PNP0C0A) in the firmware
 * ACPI namespace.  Live state is read by evaluating its _STA/_BST/_BIF
 * methods — on real laptops the EC fills those in; Windows/Linux read the
 * exact same methods.  QEMU's q35 ships no battery (Launchpad #1502613), so
 * we inject one into the firmware via an SSDT (acpi/battery.aml, loaded with
 * `-acpitable file=...`).  On real hardware this same code reads the real
 * EC battery.
 *
 * The AML support here is deliberately minimal: enough to (1) detect the
 * PNP0C0A / ACPI0003 devices in the compiled DSDT/SSDT byte stream and
 * (2) evaluate the trivial literal-package _BST/_BIF our SSDT emits.  A full
 * AML interpreter is out of scope (ACPICA is ~100k LOC); when _BST cannot be
 * evaluated we report "present, level unknown" honestly rather than guessing.
 */

/* EisaId("PNP0C0A") compiles to: 0x0C opcode + big-endian EISA dword
 * 0x41D00C0A  (ACPI 6.x EisaId packing: P=16 -> 0x41, N=14,P=16 -> 0xD0,
 * hex "0C0A" -> 0x0C 0x0A). */
static const u8 EISA_PNP0C0A[5] = { 0x0C, 0x41, 0xD0, 0x0C, 0x0A };

/* ACPI PkgLength: 1-4 bytes.  The value returned INCLUDES the PkgLength
 * bytes themselves (verified against iasl output: a Scope of 165 content
 * bytes is encoded 0x47 0x0A = 167 = 2 len bytes + 165).  nb=0: 6-bit len
 * in b0.  nb=1..3: low nibble of b0 + (nb) following bytes shifted by
 * 4,12,20. */
static int aml_pkglen(const u8 *p, const u8 *end, u64 *len, int *nb) {
    if (p >= end) return -1;
    u8 b0 = *p;
    int n = (b0 >> 6) & 0x3;
    if (n == 0) { *len = b0 & 0x3F; *nb = 1; return 0; }
    if (p + 1 + n > end) return -1;
    u64 l = b0 & 0x0F;
    for (int i = 0; i < n; i++) l |= ((u64)p[1 + i]) << (4 + 8 * i);
    *len = l; *nb = 1 + n;
    return 0;
}

/* NameString: NullName(0x00), RootChar('\' + segs), or a single 4-char
 * NameSeg.  Segs are joined with DualName '.' / MultiName '/'. */
static const u8 *aml_namestr(const u8 *p, const u8 *end) {
    if (p >= end) return NULL;
    if (*p == 0x00) return p + 1;
    if (*p == 0x5C) {                       /* root */
        p++;
        while (p < end) {
            u8 c = *p;
            if (c == 0x5E) return NULL;     /* ParentPrefix: unused here */
            if (c == 0x2E) { p++; if (p + 4 > end) return NULL; p += 4; continue; }
            if (c == 0x2F) { p++; if (p >= end) return NULL;
                              int k = *p++; if (p + 4 * k > end) return NULL;
                              p += 4 * k; continue; }
            if (p + 4 > end) return NULL;   /* NameSeg */
            p += 4;
            break;
        }
        return p;
    }
    if (p + 4 > end) return NULL;
    return p + 4;                           /* plain NameSeg */
}

/* Skip one arbitrary term.  Returns pointer past it, or NULL if we don't
 * understand the opcode. */
static const u8 *aml_skip_term(const u8 *p, const u8 *end) {
    if (p >= end) return NULL;
    u8 op = *p;
    u64 l; int nb;
    const u8 *q;
    switch (op) {
    case 0x00: case 0x01: return p + 1;                       /* Zero/One */
    case 0x0A: return (p + 2 <= end) ? p + 2 : NULL;          /* ByteConst */
    case 0x0B: return (p + 3 <= end) ? p + 3 : NULL;          /* WordConst */
    case 0x0C: return (p + 5 <= end) ? p + 5 : NULL;          /* DWordConst / EisaId */
    case 0x0E: return (p + 9 <= end) ? p + 9 : NULL;          /* QWordConst */
    case 0x0D: {                                              /* String: prefix + chars + NUL (iasl omits a pkg len here) */
        const u8 *z = p + 1;
        while (z < end && *z != 0x00) z++;
        return (z < end) ? z + 1 : NULL;
    }
    case 0x11: case 0x12: case 0x13: {                        /* Buffer / Package */
        if (aml_pkglen(p + 1, end, &l, &nb)) return NULL;
        return (p + 1 + l <= end) ? p + 1 + l : NULL;
    }
    case 0x10: case 0x14: {                                   /* Scope / Method */
        if (aml_pkglen(p + 1, end, &l, &nb)) return NULL;
        return (p + 1 + l <= end) ? p + 1 + l : NULL;
    }
    case 0x08: {                                              /* Name: NameString + arg */
        q = aml_namestr(p + 1, end);
        if (!q) return NULL;
        return aml_skip_term(q, end);
    }
    case 0x5B: {                                              /* DeviceOp etc. */
        if (p + 1 >= end) return NULL;
        u8 op2 = p[1];
        if (op2 >= 0x82 && op2 <= 0x88) {
            if (aml_pkglen(p + 2, end, &l, &nb)) return NULL;
            return (p + 2 + l <= end) ? p + 2 + l : NULL;
        }
        return NULL;
    }
    default: return NULL;
    }
}

/* Read an integer constant.  0 on success with *next past it. */
static int aml_integer(const u8 *p, const u8 *end, u64 *v, const u8 **next) {
    if (p >= end) return -1;
    switch (*p) {
    case 0x00: *v = 0; *next = p + 1; return 0;
    case 0x01: *v = 1; *next = p + 1; return 0;
    case 0x0A: if (p + 2 > end) return -1; *v = p[1]; *next = p + 2; return 0;
    case 0x0B: if (p + 3 > end) return -1; *v = p[1] | ((u64)p[2] << 8); *next = p + 3; return 0;
    case 0x0C: if (p + 5 > end) return -1;
               *v = (u64)p[1] | ((u64)p[2] << 8) | ((u64)p[3] << 16) | ((u64)p[4] << 24);
               *next = p + 5; return 0;
    case 0x0E: if (p + 9 > end) return -1;
               *v = 0; for (int i = 0; i < 8; i++) *v |= ((u64)p[1 + i]) << (8 * i);
               *next = p + 9; return 0;
    default: return -1;
    }
}

/* Parse a Package term at p (must be 0x12).  Fills up to `max` integer
 * elements (strings/buffers are skipped).  Returns pointer past the package
 * or NULL. */
static const u8 *aml_package(const u8 *p, const u8 *end, u64 *vals, int max, int *count) {
    if (p >= end || *p != 0x12) return NULL;
    u64 l; int nb;
    if (aml_pkglen(p + 1, end, &l, &nb)) return NULL;
    const u8 *c = p + 1 + nb;            /* NumElements byte */
    const u8 *pe = p + 1 + l;            /* end (pkglen value includes its own bytes) */
    if (pe > end || c >= pe) return NULL;
    u8 n = *c;
    const u8 *e = c + 1;
    int cnt = 0;
    for (u8 i = 0; i < n; i++) {
        u64 v; const u8 *ne;
        if (cnt < max && aml_integer(e, pe, &v, &ne) == 0) { vals[cnt++] = v; e = ne; }
        else { e = aml_skip_term(e, pe); if (!e) return NULL; }
    }
    if (count) *count = cnt;
    return pe;
}

/* Find a Method named `name` (4 chars) at/after `start`.  *tl = term list
 * (after flags byte), *tend = one past the method. */
static int aml_find_method(const u8 *start, const u8 *end, const char *name,
                           const u8 **tl, const u8 **tend) {
    const u8 *p = start;
    while (p + 2 <= end) {
        if (*p == 0x14) {
            u64 l; int nb;
            if (aml_pkglen(p + 1, end, &l, &nb)) { p++; continue; }
            const u8 *ns = p + 1 + nb;               /* NameString start */
            if (ns + 4 <= end && memcmp(ns, name, 4) == 0) {
                if (ns + 5 > end) return -1;
                *tl = ns + 5;                        /* term list after flags */
                *tend = p + 1 + l;                   /* end (pkglen includes its own bytes) */
                if (*tend > end) return -1;
                return 0;
            }
        }
        p++;
    }
    return -1;
}

/* Evaluate a method body looking for "Return (Package(...))". */
static int aml_method_return_package(const u8 *tl, const u8 *tend,
                                     u64 *vals, int max, int *count) {
    const u8 *p = tl;
    while (p < tend) {
        if (*p == 0xA4) {                          /* ReturnOp */
            if (aml_package(p + 1, tend, vals, max, count)) return 0;
            return -1;
        }
        const u8 *n = aml_skip_term(p, tend);
        if (!n) return -1;
        p = n;
    }
    return -1;
}

static int find_bytes(const u8 *p, u32 len, const u8 *needle, u32 nlen) {
    if (nlen == 0 || len < nlen) return -1;
    for (u32 i = 0; i + nlen <= len; i++)
        if (memcmp(p + i, needle, nlen) == 0) return (int)i;
    return -1;
}

void acpi_battery_scan(void) {
    memset(&g_acpi_battery, 0, sizeof g_acpi_battery);
    g_acpi_battery.level = -1;
    for (int t = 0; t < g_aml_n; t++) {
        const u8 *aml = g_aml_tabs[t].aml;
        u32 len = g_aml_tabs[t].len;
        /* AC adapter = the literal "ACPI0003" string HID */
        if (!g_acpi_battery.ac_present && find_bytes(aml, len, (const u8 *)"ACPI0003", 8) >= 0)
            g_acpi_battery.ac_present = true;
        /* battery = EisaId("PNP0C0A") */
        int bat_off = find_bytes(aml, len, EISA_PNP0C0A, sizeof EISA_PNP0C0A);
        if (bat_off < 0) continue;
        g_acpi_battery.present = true;
        const u8 *end = aml + len;
        const u8 *tl, *tend;
        u64 bst[4]; int nbst = 0;
        u64 bif[13]; int nbif = 0;
        if (aml_find_method(aml + bat_off, end, "_BST", &tl, &tend) == 0) {
            aml_method_return_package(tl, tend, bst, 4, &nbst);
            if (aml_find_method(tend, end, "_BIF", &tl, &tend) == 0)
                aml_method_return_package(tl, tend, bif, 13, &nbif);
        }
        if (nbst >= 4) {
            u64 state = bst[0];
            u64 remaining = bst[2];
            u64 last_full = (nbif >= 3) ? bif[2] : 0;
            u64 design = (nbif >= 2) ? bif[1] : 0;
            u64 cap = last_full ? last_full : design;
            int level = -1;
            if (cap > 0 && remaining < 0x80000000ULL)
                level = (int)(remaining * 100 / cap);
            if (level < 0) level = 0;
            if (level > 100) level = 100;
            g_acpi_battery.level = level;
            g_acpi_battery.charging = (state & 0x2) != 0;
        }
        kprintf("acpi: battery PNP0C0A present, AC %s, _BST level=%d charging=%d\n",
                g_acpi_battery.ac_present ? "online" : "absent",
                g_acpi_battery.level, g_acpi_battery.charging);
        return;
    }
    kprintf("acpi: no battery (PNP0C0A) in firmware tables - on mains / no battery\n");
}
