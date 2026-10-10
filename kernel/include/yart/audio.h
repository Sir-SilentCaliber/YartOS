#pragma once
#include <yart/types.h>

/* Intel HD Audio (row 17): a real codec command path + a DMA playback stream
 * driven through a BDL (buffer descriptor list), so the OS can actually emit
 * audio.  Verified under QEMU at the register/DMA level (a sine buffer plays
 * through the output converter and the stream's link position advances).
 */
void audio_init(void);     /* probe + bring up HDA + start a test playback */
void audio_poll(void);     /* accumulate the stream's link position        */
bool audio_present(void);
u16  audio_codec_vendor_id(void);
u16  audio_codec_device_id(void);
u64  audio_stream_position(void);   /* bytes played by the output stream  */
void audio_set_volume(int v);       /* 0..100, drives the DAC amp gain    */
int  audio_get_volume(void);        /* last value applied to the amp      */
