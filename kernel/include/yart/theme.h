/*
 * Yart OS - "Slate Amber" theme
 *
 * Calm, warm, low-saturation desktop colours.  Charcoal background, soft
 * stone window chrome, with a single warm amber accent for focus and
 * interaction.  No neon, no purple.  Inspired by quiet design systems
 * (Nord, Solarized, Tokyo Night - day variant).
 */
#pragma once

/* ----- background / surfaces ----- */
#define TH_DESKTOP_TOP   0xFF1B1F24   /* charcoal top of wallpaper        */
#define TH_DESKTOP_BOT   0xFF11141A   /* charcoal bottom                  */
#define TH_DESKTOP_DOT   0xFF222731   /* very subtle grid dot             */

#define TH_PANEL         0xFF1F242C   /* taskbar / dock body              */
#define TH_PANEL_HI      0xFF272D37   /* panel highlight                  */
#define TH_PANEL_LINE    0xFF0C0E12   /* panel hairline edge              */

#define TH_WIN_BG        0xFF20252E   /* window content area              */
#define TH_WIN_BG_ALT    0xFF252B35
#define TH_WIN_TITLE     0xFF2A313C   /* unfocused titlebar               */
#define TH_WIN_TITLE_FG  0xFFC8CDD6   /* focused titlebar                 */
#define TH_WIN_BORDER    0xFF12151B
#define TH_WIN_BORDER_F  0xFFE8A87C   /* amber accent border when focused */
#define TH_WIN_SHADOW    0x66000000

/* ----- text ----- */
#define TH_TEXT          0xFFE6E8EE
#define TH_TEXT_DIM      0xFF8B93A0
#define TH_TEXT_MUTED    0xFF5A626E
#define TH_TEXT_INV      0xFF11141A

/* ----- accents ----- */
#define TH_ACCENT        0xFFE8A87C   /* warm amber                       */
#define TH_ACCENT_DIM    0xFFB07A55
#define TH_ACCENT_BG     0xFF3A2E26   /* amber tinted dark surface        */
#define TH_OK            0xFF8FBC8F
#define TH_WARN          0xFFE0C088
#define TH_ERR           0xFFCC7777

/* ----- traffic-light buttons ----- */
#define TH_BTN_CLOSE     0xFFCC7777
#define TH_BTN_MIN       0xFFE0C088
#define TH_BTN_MAX       0xFF8FBC8F
#define TH_BTN_DIM       0xFF3A4049

/* ----- editor / terminal ----- */
#define TH_EDITOR_BG     0xFF15181E
#define TH_EDITOR_FG     0xFFD8DCE3
#define TH_EDITOR_GUTTER 0xFF1B1F26
#define TH_EDITOR_LINE   0xFF5C6370
#define TH_EDITOR_SEL    0xFF3A4554
#define TH_EDITOR_CARET  0xFFE8A87C
