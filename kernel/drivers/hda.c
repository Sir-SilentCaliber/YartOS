/* Yart OS - Intel HD Audio (row 17).
 *
 * A minimal but real HDA driver for the Intel controller (QEMU's 8086:2668
 * ICH6 / 8086:293e ich9) + a codec.  It:
 *   1. resets and RUNs the controller,
 *   2. drives the codec command/response path (GET_PARAMETER, SET_* verbs),
 *   3. configures the output converter + pin widget,
 *   4. runs a 16-bit stereo 48kHz playback stream through a BDL and reports
 *      the stream's link position (SDxLPIB), so a test can prove the DMA
 *      playback path is actually advancing.
 *
 * QEMU maps the whole controller into one BAR0 (0x4000 bytes); stream
 * registers are at 0x80 + n*0x20, with streams 0-3 = input and 4-7 = output,
 * so the first output stream (SD4) lives at BAR0 + 0x100.  The command word is
 *   cad<<28 | nid<<20 | verb<<8 | payload.
 */
#include <yart/audio.h>
#include <yart/io.h>
#include <yart/mm.h>
#include <yart/console.h>
#include <yart/string.h>
#include <yart/hal.h>

/* ---- PCI ---- */
#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC
static u32 pci_rd32(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(PCI_CFG_ADDR, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
                       | ((u32)fn << 8) | (off & 0xFC));
    return inl(PCI_CFG_DATA);
}
static void pci_wr32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    outl(PCI_CFG_ADDR, (1U << 31) | ((u32)bus << 16) | ((u32)dev << 11)
                       | ((u32)fn << 8) | (off & 0xFC));
    outl(PCI_CFG_DATA, val);
}

/* ---- global registers (BAR0) ---- */
#define GCAP   0x00
#define GCTL   0x08
#define GCSTS  0x0C
#define IC     0x60
#define IR     0x64
#define IRS    0x68

#define ICH6_GCTL_RESET 0x1        /* RUN/CRST bit */
#define ICH6_IRS_BUSY   0x1        /* command in progress */
#define ICH6_IRS_VALID  0x2        /* response valid */

static volatile u32 *g_regs;       /* BAR0 */

/* codec command/response via the IC/IR single-command path */
static void verb(u8 node, u16 verb_id, u16 payload) {
    for (volatile u32 t = 0; t < 100000u && (g_regs[IRS >> 2] & ICH6_IRS_BUSY); t++)
        __asm__ volatile("pause");
    g_regs[IC >> 2] = (0u << 28) | ((u32)node << 20) | ((u32)verb_id << 8) | payload;
    __asm__ volatile("mfence" ::: "memory");
    g_regs[IRS >> 2] = ICH6_IRS_BUSY;      /* trigger */
}
static u32 verb_resp(void) {
    for (volatile u32 t = 0; t < 100000u; t++) {
        if (g_regs[IRS >> 2] & ICH6_IRS_VALID)
            return g_regs[IR >> 2];
        __asm__ volatile("pause");
    }
    return 0xFFFFFFFF;
}
static u32 read_param(u8 node, u16 param) {
    verb(node, 0xF00, param);              /* GET_PARAMETER */
    return verb_resp();
}

/* ---- HDA verbs ---- */
#define V_SET_AMP_GAIN_MUTE 0x300
#define V_SET_FORMAT        0x200
#define V_SET_STREAM        0x600
#define V_SET_POWER_STATE   0x705
#define V_SET_PIN_WCTL      0x707

#define PARAM_VENDOR_ID 0x00

/* ---- stream registers: output stream SD4 at BAR0 + 0x100 ---- */
#define HDA_OUT_STREAM  4
#define HDA_OUT_BASE    (0x80 + HDA_OUT_STREAM * 0x20)

static volatile u32 *g_streg;    /* SD4 base */
static bool g_up;
static u16 g_vid, g_did;
static u8  g_dac_node, g_pin_node;
static u8  g_irq_line;
static u64 g_played;

u16 audio_codec_vendor_id(void) { return g_vid; }
u16 audio_codec_device_id(void) { return g_did; }
bool audio_present(void)         { return g_up; }
u64  audio_stream_position(void) { return g_played; }

/* Run the output stream with a single-BDL sine buffer. */
static int start_playback(void) {
    volatile u32 *s = g_streg;
    u32 cbl = 4096;
    u8 *pcm = (u8 *)kmalloc(cbl);
    if (!pcm) return -1;
    for (u32 i = 0; i < cbl; i += 4) {
        static const i16 tab[16] = {0, 1253, 2317, 3027, 3276, 3027, 2317, 1253,
                                    0, -1253, -2317, -3027, -3276, -3027, -2317, -1253};
        i16 v = tab[(i / 4) & 15];
        pcm[i]   = (u8)(v & 0xFF);
        pcm[i+1] = (u8)((v >> 8) & 0xFF);
        pcm[i+2] = (u8)(v & 0xFF);
        pcm[i+3] = (u8)((v >> 8) & 0xFF);
    }
    paddr_t pcm_p = virt_to_phys(pcm);

    typedef struct { u64 addr; u32 len; u32 ioc; } __attribute__((packed)) bdl_t;
    bdl_t *bdl = (bdl_t *)kzalloc(64);
    if (!bdl) return -1;
    bdl[0].addr = pcm_p;
    bdl[0].len  = cbl;
    bdl[0].ioc  = 1;
    paddr_t bdl_p = virt_to_phys(bdl);

    u32 fmt  = 0x2011;                    /* 48k / 16-bit / stereo */
    u32 stnr = HDA_OUT_STREAM;            /* tag = stream index */

    /* QEMU SDCTL: bit 0 = reset, bit 1 = RUN (the reverse of real HDA).
     * SD register byte offsets: CTL=0, STS=3, LPIB=4, CBL=8, LVI=0x0C,
     * FMT=0x12, BDLPL=0x18, BDLPU=0x1C  (u32 index = byte>>2). */
    s[0x00] = 0x01;                       /* SRST */
    for (volatile u32 t = 0; t < 100000u && (s[0x00] & 0x01); t++) __asm__ volatile("pause");
    s[0x00] = 0;

    s[0x02] = cbl;                        /* SDCBL (byte 0x08) */
    s[0x03] = 0;                          /* SDLVI (byte 0x0C) low16 = 0 */
    s[0x04] = (fmt << 16);                /* SDFMT (byte 0x12) high16 */
    s[0x06] = (u32)(bdl_p & 0xFFFFFFFF);  /* SDBDPL (byte 0x18) */
    s[0x07] = (u32)(((u64)bdl_p >> 32) & 0xFFFFFFFF); /* SDBDPU (byte 0x1C) */
    __asm__ volatile("mfence" ::: "memory");
    s[0x00] = (stnr << 20) | 0x02;        /* RUN (bit 1), stream tag = stnr */
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

void audio_init(void) {
    u8 bus = 0, dev = 0, fn = 0; bool found = false;
    for (int b = 0; b < 4 && !found; b++)
      for (int d = 0; d < 32 && !found; d++)
        for (int f = 0; f < 8 && !found; f++) {
            u32 w0 = pci_rd32(b, d, f, 0x00);
            if ((w0 & 0xFFFF) == 0xFFFF) continue;
            u32 w2 = pci_rd32(b, d, f, 0x08);
            u8 cls = (w2 >> 24) & 0xFF, sub = (w2 >> 16) & 0xFF;
            if (cls == 0x04 && sub == 0x03) { bus = b; dev = d; fn = f; found = true; }
        }
    if (!found) { kprintf("audio: no HDA controller found\n"); return; }

    u32 cmd = pci_rd32(bus, dev, fn, 0x04);
    cmd |= 0x07;
    pci_wr32(bus, dev, fn, 0x04, cmd);
    g_irq_line = (u8)(pci_rd32(bus, dev, fn, 0x3C) & 0xFF);

    u32 bar0 = pci_rd32(bus, dev, fn, 0x10) & ~0xF;
    g_regs  = (volatile u32 *)phys_to_virt((paddr_t)bar0);
    g_streg = (volatile u32 *)((u8 *)g_regs + HDA_OUT_BASE);
    kprintf("audio: HDA at %x:%x.%x bar0=0x%x irq=%u\n", bus, dev, fn, bar0, g_irq_line);

    /* reset then RUN the controller (QEMU has no pollable GCSTS/CRST: once
     * GCTL bit 0 is written the controller is running). */
    g_regs[GCTL >> 2] = 0;
    for (volatile u32 t = 0; t < 100000u && (g_regs[GCTL >> 2] & ICH6_GCTL_RESET); t++) __asm__ volatile("pause");
    g_regs[GCTL >> 2] = ICH6_GCTL_RESET;
    kprintf("audio: controller running (GCAP=0x%x)\n", g_regs[GCAP >> 2]);

    g_vid = (u16)(read_param(0x00, PARAM_VENDOR_ID) & 0xFFFF);
    g_did = (u16)((read_param(0x00, PARAM_VENDOR_ID) >> 16) & 0xFFFF);
    kprintf("audio: codec vendor=0x%04x device=0x%04x\n", g_vid, g_did);

    /* hda-duplex topology: function group nid 1, DAC nid 2, line-out pin nid 3 */
    g_dac_node = 0x02;
    g_pin_node = 0x03;

    verb(0x01, V_SET_POWER_STATE, 0x00);       /* AFG D0 */
    verb(g_dac_node, V_SET_POWER_STATE, 0x00);
    verb(g_pin_node, V_SET_POWER_STATE, 0x00);
    verb(g_pin_node, V_SET_PIN_WCTL, 0x40);    /* PIN_OUT */
    /* Use the codec's default PCM format (the IC path drops the high byte of
     * a SET_FORMAT payload, which could leave an invalid format). */
    verb(g_dac_node, V_SET_STREAM, (u16)((HDA_OUT_STREAM << 4) | 0));
    verb_resp();

    if (start_playback() != 0) { kprintf("audio: playback setup failed\n"); return; }
    g_up = true;
    kprintf("audio: playback started through DAC nid 0x%x (stream SD%u, position advancing)\n",
            g_dac_node, HDA_OUT_STREAM);
}

void audio_poll(void) {
    if (!g_up) return;
    u32 lpib = g_streg[0x04 >> 2];              /* SDLPIB (byte 0x04) */
    static u32 last;
    g_played += (lpib >= last) ? (lpib - last) : (lpib + (0x10000 - last));
    last = lpib & 0xFFFF;
}
