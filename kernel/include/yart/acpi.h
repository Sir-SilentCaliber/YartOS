#pragma once
#include <yart/types.h>

/* ISA interrupt source overrides from the MADT */
typedef struct {
    u8  irq;    /* ISA IRQ number                     */
    u32 gsi;    /* global system interrupt            */
    u16 flags;  /* bit0 = polarity, bit1 = trigger    */
} madt_iso_t;

/* What we extract from the MADT for the APIC subsystem */
typedef struct {
    u8  apic_id;
    u8  uid;
} lapic_entry_t;

typedef struct {
    bool  present;
    u32   lapic_addr;
    u32   ioapic_addr;
    lapic_entry_t lapics[16];
    int   lapic_count;
    u32   ioapic_gsi_base;
    madt_iso_t override[16];
    int   override_count;
} madt_info_t;

extern madt_info_t g_madt;
void acpi_init(void *rsdp);

/* ACPI Control-Method Battery (PNP0C0A) + AC adapter (ACPI0003).
 * Populated from the firmware DSDT/SSDTs by acpi_battery_scan().
 * This is the exact mechanism Windows/Linux use to read battery state. */
typedef struct {
    bool present;      /* PNP0C0A battery device found in the namespace   */
    bool ac_present;   /* ACPI0003 AC adapter device found                */
    bool charging;     /* from _BST state (bit 1)                         */
    int  level;        /* 0..100 from _BST remaining / _BIF design, -1 = ?*/
} acpi_battery_t;

extern acpi_battery_t g_acpi_battery;
void acpi_battery_scan(void);
