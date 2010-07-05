/*
 * Copyright (C) 2010 Benjamin Zores <ben@geexbox.org>
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Blu-ray parser/reader using libbluray
 *  Use 'git clone git://git.videolan.org/libbluray' to get it.
 *
 * TODO:
 *  - Add libbdnav support for menus navigation
 *  - Add AACS/BD+ protection detection
 *  - Add descrambled keys database support (KEYDB.cfg)
 *
 */

#include <libbluray/bluray.h>

#include "config.h"
#include "libavutil/common.h"
#include "libmpdemux/demuxer.h"
#include "mp_msg.h"
#include "m_struct.h"
#include "m_option.h"
#include "stream.h"

#define BLURAY_SECTOR_SIZE     6144

#define BLURAY_DEFAULT_ANGLE      0
#define BLURAY_DEFAULT_CHAPTER    0
#define BLURAY_DEFAULT_TITLE      0

char *bluray_device  = NULL;
int   bluray_angle   = 0;
int   bluray_chapter = 0;

static struct stream_priv_s {
    int title;
    char *device;
} bluray_stream_priv_dflts = {
    BLURAY_DEFAULT_TITLE,
    NULL
};

#define ST_OFF(f) M_ST_OFF(struct stream_priv_s,f)
static const m_option_t bluray_stream_opts_fields[] = {
    { "hostname", ST_OFF(title),  CONF_TYPE_INT, M_OPT_RANGE, 0, 99999, NULL},
    { "filename", ST_OFF(device), CONF_TYPE_STRING, 0, 0 ,0, NULL},
    { NULL, NULL, 0, 0, 0, 0,  NULL }
};

static const struct m_struct_st bluray_stream_opts = {
    "bluray",
    sizeof(struct stream_priv_s),
    &bluray_stream_priv_dflts,
    bluray_stream_opts_fields
};

static void bluray_stream_close(stream_t *s)
{
    bd_close(s->priv);
    s->priv = NULL;
}

static int bluray_stream_seek(stream_t *s, off_t pos)
{
    off_t p;

    p = bd_seek(s->priv, pos);
    if (p == -1)
        return 0;

    s->pos = p;
    return 1;
}

static int bluray_stream_fill_buffer(stream_t *s, char *buf, int len)
{
    return bd_read(s->priv, buf, len);
}

static int bluray_stream_open(stream_t *s, int mode,
                              void *opts, int *file_format)
{
    struct stream_priv_s *p = opts;
    BLURAY_TITLE_INFO *info = NULL;
    BLURAY *bd;

    int title, title_guess, title_count;
    uint64_t title_size;

    unsigned int chapter, angle;
    uint64_t max_duration = 0;
    int64_t chapter_pos = 0;

    char *device = NULL;
    int i;

    /* find the requested device */
    if (p->device)
        device = p->device;
    else if (bluray_device)
        device = bluray_device;

    if (!device) {
        mp_tmsg(MSGT_OPEN, MSGL_ERR,
                "No Blu-ray device/location was specified ...\n");
        return STREAM_UNSUPPORTED;
    }

    /* open device */
    bd = bd_open(device, NULL);
    if (!bd) {
        mp_tmsg(MSGT_OPEN, MSGL_ERR, "Couldn't open Blu-ray device: %s\n",
               device);
        return STREAM_UNSUPPORTED;
    }

    /* check for available titles on disc */
    title_count = bd_get_titles(bd, TITLES_RELEVANT);
    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_BLURAY_TITLES=%d\n", title_count);
    if (!title_count) {
        mp_msg(MSGT_OPEN, MSGL_ERR,
               "Can't find any Blu-ray-compatible title here.\n");
        bd_close(bd);
        return STREAM_UNSUPPORTED;
    }

    /* parse titles information */
    title_guess = BLURAY_DEFAULT_TITLE;
    for (i = 0; i < title_count; i++) {
        BLURAY_TITLE_INFO *ti;
        int sec, msec;

        ti = bd_get_title_info(bd, i);
        if (!ti)
            continue;

        sec  = ti->duration / 90000;
        msec = (ti->duration - sec) % 1000;

        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_BLURAY_TITLE_%d_CHAPTERS=%d\n", i + 1, ti->chapter_count);
        mp_msg(MSGT_IDENTIFY, MSGL_INFO,
               "ID_BLURAY_TITLE_%d_ANGLE=%d\n", i + 1, ti->angle_count);
        mp_msg(MSGT_IDENTIFY, MSGL_V,
               "ID_BLURAY_TITLE_%d_LENGTH=%d.%03d\n", i + 1, sec, msec);

        /* try to guess which title may contain the main movie */
        if (ti->duration > max_duration) {
            max_duration = ti->duration;
            title_guess = i;
        }

        bd_free_title_info(ti);
    }

    /* Select current title */
    title = p->title ? p->title - 1: title_guess;
    title = FFMIN(title, title_count - 1);

    bd_select_title(bd, title);

    title_size = bd_get_title_size(bd);
    mp_msg(MSGT_IDENTIFY, MSGL_INFO,
           "ID_BLURAY_CURRENT_TITLE=%d\n", title + 1);

    /* Get current title information */
    info = bd_get_title_info(bd, title);
    if (!info)
        goto err_no_info;

    /* Select chapter */
    chapter = bluray_chapter ? bluray_chapter : BLURAY_DEFAULT_CHAPTER;
    chapter = FFMIN(chapter, info->chapter_count);

    if (chapter)
        chapter_pos = bd_chapter_pos(bd, chapter);

    mp_msg(MSGT_IDENTIFY, MSGL_INFO,
           "ID_BLURAY_CURRENT_CHAPTER=%d\n", chapter + 1);

    /* Select angle */
    angle = bluray_angle ? bluray_angle : BLURAY_DEFAULT_ANGLE;
    angle = FFMIN(angle, info->angle_count);

    if (angle)
        bd_select_angle(bd, angle);

    mp_msg(MSGT_IDENTIFY, MSGL_INFO, "ID_BLURAY_CURRENT_ANGLE=%d\n", angle + 1);

    bd_free_title_info(info);

err_no_info:
    s->fill_buffer = bluray_stream_fill_buffer;
    s->seek        = bluray_stream_seek;
    s->close       = bluray_stream_close;

    s->start_pos   = chapter_pos;
    s->end_pos     = title_size;
    s->sector_size = BLURAY_SECTOR_SIZE;
    s->flags       = mode | MP_STREAM_SEEK;
    s->priv        = bd;
    s->type        = STREAMTYPE_BLURAY;
    s->url         = strdup("br://");

    mp_tmsg(MSGT_OPEN, MSGL_V, "Blu-ray successfully opened.\n");

    return STREAM_OK;
}

const stream_info_t stream_info_bluray = {
    "Blu-ray Disc",
    "bd",
    "Benjamin Zores",
    "Play Blu-ray discs through external libbluray",
    bluray_stream_open,
    { "br", "bluray", NULL },
    &bluray_stream_opts,
    1
};
