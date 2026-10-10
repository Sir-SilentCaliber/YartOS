/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20250404 (64-bit version)
 * Copyright (c) 2000 - 2025 Intel Corporation
 * 
 * Disassembling to symbolic ASL+ operators
 *
 * Disassembly of battery.aml
 *
 * Original Table Header:
 *     Signature        "SSDT"
 *     Length           0x000000CC (204)
 *     Revision         0x02
 *     Checksum         0xD0
 *     OEM ID           "YARTOS"
 *     OEM Table ID     "BATTERY"
 *     OEM Revision     0x00000001 (1)
 *     Compiler ID      "INTL"
 *     Compiler Version 0x20250404 (539296772)
 */
DefinitionBlock ("", "SSDT", 2, "YARTOS", "BATTERY", 0x00000001)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A") /* Control Method Battery */)  // _HID: Hardware ID
            Name (_UID, One)  // _UID: Unique ID
            Name (_STA, 0x1F)  // _STA: Status
            Method (_BST, 0, NotSerialized)  // _BST: Battery Status
            {
                Return (Package (0x04)
                {
                    Zero, 
                    0xFFFFFFFF, 
                    0x1E50, 
                    0x2A30
                })
            }

            Method (_BIF, 0, NotSerialized)  // _BIF: Battery Information
            {
                Return (Package (0x0D)
                {
                    Zero, 
                    0x24B8, 
                    0x24B8, 
                    One, 
                    0x2A30, 
                    0x03E8, 
                    0x01F4, 
                    Zero, 
                    Zero, 
                    "YartBat", 
                    "SN-001", 
                    "LiIon", 
                    "YartOS"
                })
            }
        }

        Device (AC0)
        {
            Name (_HID, "ACPI0003" /* Power Source Device */)  // _HID: Hardware ID
            Name (_UID, One)  // _UID: Unique ID
            Name (_STA, 0x0F)  // _STA: Status
            Method (_PSR, 0, NotSerialized)  // _PSR: Power Source
            {
                Return (One)
            }
        }
    }
}

