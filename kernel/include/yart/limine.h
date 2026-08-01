/*
 * Yart OS - Slim vendored copy of the Limine boot protocol (subset we use).
 * Original: https://github.com/limine-bootloader/limine
 * License: BSD-2-Clause (upstream).
 *
 * We only declare what Yart actually consumes: framebuffer, memmap, HHDM,
 * modules, RSDP, and bootloader info.  This header is intentionally small
 * so the kernel does not have to depend on the full upstream tree.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LIMINE_PTR(TYPE) TYPE

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

/* ---------- request markers (placed in special sections by linker) ---------- */
#define LIMINE_REQUESTS_START_MARKER \
    __attribute__((used, section(".limine_requests_start"))) \
    static volatile uint64_t limine_requests_start_marker[4] = { \
        0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf, \
        0x785c6ed015d3e316, 0x181e920a7852b9d9 }

#define LIMINE_REQUESTS_END_MARKER \
    __attribute__((used, section(".limine_requests_end"))) \
    static volatile uint64_t limine_requests_end_marker[2] = { \
        0xadc0e0531bb10d03, 0x9572709f31764c62 }

#define LIMINE_BASE_REVISION(N) \
    __attribute__((used, section(".limine_requests"))) \
    static volatile uint64_t limine_base_revision[3] = { \
        0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (N) }

/* ---------- framebuffer ---------- */
struct limine_video_mode {
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
};

struct limine_framebuffer {
    LIMINE_PTR(void *) address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t  memory_model;
    uint8_t  red_mask_size;
    uint8_t  red_mask_shift;
    uint8_t  green_mask_size;
    uint8_t  green_mask_shift;
    uint8_t  blue_mask_size;
    uint8_t  blue_mask_shift;
    uint8_t  unused[7];
    uint64_t edid_size;
    LIMINE_PTR(void *) edid;
    /* response rev >= 1 */
    uint64_t mode_count;
    LIMINE_PTR(struct limine_video_mode **) modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    LIMINE_PTR(struct limine_framebuffer **) framebuffers;
};

#define LIMINE_FRAMEBUFFER_REQUEST { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b }
struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_framebuffer_response *) response;
};

/* ---------- memmap ---------- */
#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    LIMINE_PTR(struct limine_memmap_entry **) entries;
};

#define LIMINE_MEMMAP_REQUEST { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62 }
struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_memmap_response *) response;
};

/* ---------- HHDM (higher-half direct map) ---------- */
struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

#define LIMINE_HHDM_REQUEST { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b }
struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_hhdm_response *) response;
};

/* ---------- modules (initrd) ---------- */
struct limine_file {
    uint64_t revision;
    LIMINE_PTR(void *) address;
    uint64_t size;
    LIMINE_PTR(char *) path;
    LIMINE_PTR(char *) cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    uint8_t  gpt_disk_uuid[16];
    uint8_t  gpt_part_uuid[16];
    uint8_t  part_uuid[16];
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    LIMINE_PTR(struct limine_file **) modules;
};

#define LIMINE_MODULE_REQUEST { LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee }
struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_module_response *) response;
    /* rev >= 1 internal modules omitted */
};

/* ---------- bootloader info ---------- */
struct limine_bootloader_info_response {
    uint64_t revision;
    LIMINE_PTR(char *) name;
    LIMINE_PTR(char *) version;
};

#define LIMINE_BOOTLOADER_INFO_REQUEST { LIMINE_COMMON_MAGIC, 0xf55038d8e2a1202f, 0x279426fcf5f59740 }
struct limine_bootloader_info_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_bootloader_info_response *) response;
};

/* ---------- RSDP ---------- */
struct limine_rsdp_response {
    uint64_t revision;
    LIMINE_PTR(void *) address;
};

#define LIMINE_RSDP_REQUEST { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43, 0x27637845accdcf3c }
struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_rsdp_response *) response;
};

/* ---------- SMP (bootloader-started APs) ---------- */
#define LIMINE_SMP_REQUEST { LIMINE_COMMON_MAGIC, 0x95a67b819a1b857e, 0xa0b61b723b6a73e0 }

struct limine_smp_info;
typedef void (*limine_goto_address)(struct limine_smp_info *);

#define LIMINE_SMP_X2APIC (1 << 0)

struct limine_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    LIMINE_PTR(limine_goto_address) goto_address;
    uint64_t extra_argument;
};

struct limine_smp_response {
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    LIMINE_PTR(struct limine_smp_info **) cpus;
};

struct limine_smp_request {
    uint64_t id[4];
    uint64_t revision;
    LIMINE_PTR(struct limine_smp_response *) response;
    uint64_t flags;
};
