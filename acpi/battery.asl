/*
 * battery.asl — virtual firmware battery for QEMU guests.
 *
 * QEMU's q35 machine ships NO battery device (Launchpad bug #1502613; the
 * QEMU-devel "QEMU Battery" patch wasn't merged until after 10.0), so a real
 * OS booted in QEMU sees "no battery, on AC".  This SSDT is the same trick
 * people use in the real world to give a VM a battery: it injects a standard
 * ACPI Control-Method Battery (PNP0C0A) + AC adapter (ACPI0003) into the
 * firmware tables via `-acpitable file=battery.aml`.
 *
 * YartOS then reads it through the exact same path Windows/Linux use:
 * ACPI namespace scan -> _STA / _BST / _BIF method evaluation.
 * The charge is static because there is no physical battery controller to
 * sample; on real laptop hardware the same code reads the real EC battery.
 */
DefinitionBlock ("battery.aml", "SSDT", 2, "YARTOS", "BATTERY", 0x00000001)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, 1)
            Name (_STA, 0x1F)

            Method (_BST, 0, NotSerialized)
            {
                Return (Package (0x04)
                {
                    0x00000000,    /* state: 0 = neither charging nor discharging (plugged) */
                    0xFFFFFFFF,    /* present rate: unknown */
                    0x00001E50,    /* remaining capacity: 7760 mWh */
                    0x00002A30     /* present voltage: 10800 mV */
                })
            }

            Method (_BIF, 0, NotSerialized)
            {
                Return (Package (0x0D)
                {
                    0,             /* power unit: 0 = mWh */
                    0x000024B8,    /* design capacity: 9400 mWh */
                    0x000024B8,    /* last full charge: 9400 mWh */
                    1,             /* technology: 1 = rechargeable */
                    0x00002A30,    /* design voltage: 10800 mV */
                    0x000003E8,    /* design capacity of warning: 1000 */
                    0x000001F4,    /* design capacity of low: 500 */
                    0,             /* granularity 1 */
                    0,             /* granularity 2 */
                    "YartBat",     /* model number */
                    "SN-001",      /* serial number */
                    "LiIon",       /* battery type */
                    "YartOS"       /* OEM information */
                })
            }
        }

        Device (AC0)
        {
            Name (_HID, "ACPI0003")
            Name (_UID, 1)
            Name (_STA, 0x0F)
            Method (_PSR, 0, NotSerialized)
            {
                Return (1)         /* AC power present */
            }
        }
    }
}
