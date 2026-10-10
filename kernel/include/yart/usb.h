#pragma once
#include <yart/types.h>

/* USB xHCI host controller + HID keyboard (row 18).
 *
 * Targets QEMU's qemu-xhci + usb-kbd.  A minimal but real xHCI driver:
 *   - detect the controller (PCI class 0x0C / subclass 0x03),
 *   - reset + RUN it,
 *   - enumerate the root-hub ports and address a HID keyboard,
 *   - read its interrupt IN reports and feed keycodes into the kernel's
 *     keyboard input path (the same kbd_poll_event() the desktop drains).
 *
 * The HID keyboard produces 8-byte boot-protocol reports:
 *   [0]=modifiers  [1]=reserved  [2..7]=keycodes (up to 6 simultaneous).
 */
void usb_init(void);            /* probe + bring up xHCI + enumerate HID  */
void usb_hid_poll(void);        /* poll the HID keyboard for reports      */
int  usb_address_device(u32 slot, u32 port);   /* Address Device cmd (0/1) */
void usb_configure_endpoint(u32 slot);         /* Configure Endpoint cmd  */
bool usb_kbd_present(void);
/* Feed one USB-HID keyboard event into the PS/2-style keyboard queue.
 * `mods` = [0] modifiers byte, `keycode` = HID usage (2..6). */
void usb_kbd_event(u8 mods, u8 keycode, bool released);
