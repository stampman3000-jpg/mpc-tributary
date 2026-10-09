/* Channel maps for Elektron Overbridge devices.
 * Copyright (C) 2026 Stampman3000
 * This program is free software under the GNU General Public License, version 3.
 *
 * Offsets, sample sizes and channel names are facts taken from Overwitch's
 * device list (https://github.com/dagargo/overwitch, Copyright David García Goñi,
 * GPL-3). This is not Overwitch source. Digitone II is the only map that has
 * been heard on hardware. The others are here so a SOURCE knob can name them,
 * and they are marked untested. Analog Keys is omitted: Overwitch drives that
 * machine with the older isochronous link, which this plugin does not speak.
 */
#ifndef TRIBUTARY_DEVICES_H
#define TRIBUTARY_DEVICES_H
#include <stddef.h>

#define TB_MAX 21

typedef struct {
    const char *name;
    unsigned short off;
    unsigned char bytes;  /* 4-byte slot, or 3-byte slot */
    unsigned char stereo; /* 0: one channel, copied to left and right */
} tb_src;

typedef struct {
    unsigned short pid;
    unsigned char tested;
    unsigned char nsrc;
    unsigned short frame;    /* one device-to-host frame, in bytes */
    unsigned short inframe;  /* one host-to-device frame, in bytes */
    const char *name;
    const tb_src *src;
} tb_dev;

static const tb_src tb_src_0[] = {
    {"main", 0, 4, 1},
    {"track 1", 8, 4, 1},
    {"track 2", 16, 4, 1},
    {"track 3", 24, 4, 1},
    {"track 4", 32, 4, 1},
    {"track 5", 40, 4, 1},
    {"track 6", 48, 4, 1},
    {"track 7", 56, 3, 1},
    {"track 8", 62, 3, 1},
    {"track 9", 68, 3, 1},
    {"track 10", 74, 3, 1},
    {"track 11", 80, 3, 1},
    {"track 12", 86, 3, 1},
    {"track 13", 92, 3, 1},
    {"track 14", 98, 3, 1},
    {"track 15", 104, 3, 1},
    {"track 16", 110, 3, 1},
    {"delay", 116, 3, 1},
    {"reverb", 122, 3, 1},
    {"chorus", 128, 3, 1},
    {"input", 134, 3, 1},
};

static const tb_src tb_src_1[] = {
    {"main", 0, 4, 1},
    {"track 1", 8, 4, 0},
    {"track 2", 12, 4, 0},
    {"track 3", 16, 4, 0},
    {"track 4", 20, 4, 0},
    {"track 5", 24, 4, 0},
    {"track 6", 28, 4, 0},
    {"track 7", 32, 4, 0},
    {"track 8", 36, 4, 0},
    {"input", 40, 4, 1},
};

static const tb_src tb_src_2[] = {
    {"main", 0, 4, 1},
    {"track 1", 8, 4, 1},
    {"track 2", 16, 4, 1},
    {"track 3", 24, 4, 1},
    {"track 4", 32, 4, 1},
    {"input", 40, 4, 1},
};

static const tb_src tb_src_3[] = {
    {"main", 0, 4, 1},
    {"synth track 1", 8, 4, 0},
    {"synth track 2", 12, 4, 0},
    {"synth track 3", 16, 4, 0},
    {"synth track 4", 20, 4, 0},
    {"input", 24, 4, 1},
};

static const tb_src tb_src_4[] = {
    {"main", 0, 4, 1},
    {"bd", 8, 4, 0},
    {"sd", 12, 4, 0},
    {"rs/cp", 16, 4, 0},
    {"bt", 20, 4, 0},
    {"lt", 24, 4, 0},
    {"mt/ht", 28, 4, 0},
    {"ch/oh", 32, 4, 0},
    {"cy/cb", 36, 4, 0},
    {"input", 40, 4, 1},
};

static const tb_src tb_src_5[] = {
    {"main", 0, 4, 1},
    {"fx return", 8, 4, 1},
};

static const tb_src tb_src_6[] = {
    {"main", 0, 4, 1},
    {"track 1", 8, 4, 0},
    {"track 2", 12, 4, 0},
    {"track 3", 16, 4, 0},
    {"track 4", 20, 4, 0},
    {"track 5", 24, 4, 0},
    {"track 6", 28, 4, 0},
    {"track 7", 32, 4, 0},
    {"track 8", 36, 4, 0},
    {"track 9", 40, 4, 0},
    {"track 10", 44, 4, 0},
    {"track 11", 48, 4, 0},
    {"track 12", 52, 4, 0},
    {"analog fx", 56, 4, 1},
    {"delay/reverb", 64, 4, 1},
    {"input", 72, 4, 1},
};

static const tb_dev tb_devices[] = {
    {0x0b2b, 0, 21, 140, 32, "Digitakt II", tb_src_0},
    {0x0b34, 1, 21, 140, 32, "Digitone II", tb_src_0},
    {0x0b2c, 0, 10, 48, 8, "Digitakt", tb_src_1},
    {0x0b36, 0, 6, 48, 8, "Digitone", tb_src_2},
    {0x0b35, 0, 6, 48, 8, "Digitone Keys", tb_src_2},
    {0x0b47, 0, 6, 32, 24, "Analog Four MKII", tb_src_3},
    {0x0b48, 0, 10, 48, 48, "Analog Rytm MKII", tb_src_4},
    {0x0b50, 0, 2, 16, 16, "Analog Heat", tb_src_5},
    {0x0b52, 0, 2, 16, 16, "Analog Heat MKII", tb_src_5},
    {0x0b53, 0, 2, 16, 16, "Analog Heat +FX", tb_src_5},
    {0x0b4a, 0, 16, 80, 32, "Syntakt", tb_src_6},
};

static inline const tb_dev *tb_by_pid(int pid) {
    for (size_t i = 0; i < sizeof tb_devices / sizeof tb_devices[0]; i++)
        if (tb_devices[i].pid == (unsigned short)pid) return &tb_devices[i];
    return 0;
}

/* A block is the 32-byte head plus 7 frames. */
static inline int tb_in_block(const tb_dev *d) { return d ? 32 + 7 * (int)d->frame : 0; }
static inline int tb_out_block(const tb_dev *d) { return d ? 32 + 7 * (int)d->inframe : 0; }

#endif
