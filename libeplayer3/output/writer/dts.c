/*
 * linuxdvb output/writer handling.
 *
 * konfetti 2010 based on linuxdvb.c code from libeplayer2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

/* ***************************** */
/* Includes                      */
/* ***************************** */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/stm_ioctls.h>
#include <memory.h>
#include <asm/types.h>
#include <pthread.h>
#include <errno.h>

#include "common.h"
#include "output.h"
#include "debug.h"
#include "misc.h"
#include "pes.h"
#include "writer.h"

/* ***************************** */
/* Makros/Constants              */
/* ***************************** */
#define PES_AUDIO_PRIVATE_HEADER_SIZE   16                                // consider maximum private header size.
#define PES_AUDIO_HEADER_SIZE           (32 + PES_AUDIO_PRIVATE_HEADER_SIZE)
#define PES_AUDIO_PACKET_SIZE           2028
#define SPDIF_AUDIO_PACKET_SIZE         (1024 * sizeof(unsigned int) * 2) // stereo 32bit samples.

#define DTS_DEBUG

#ifdef DTS_DEBUG

static short debug_level = 0;
static const char *FILENAME = "dts.c";

#define dts_printf(level, fmt, x...) do { \
if (debug_level >= level) printf("[%s:%s] " fmt, FILENAME, __FUNCTION__, ## x); } while (0)
#else
#define dts_printf(level, fmt, x...)
#endif

#ifndef DTS_SILENT
#define dts_err(fmt, x...) do { printf("[%s:%s] " fmt, FILENAME, __FUNCTION__, ## x); } while (0)
#else
#define dts_err(fmt, x...)
#endif

/* ***************************** */
/* Types                         */
/* ***************************** */

/* ***************************** */
/* Varaibles                     */
/* ***************************** */

/* ***************************** */
/* Prototypes                    */
/* ***************************** */

/* ***************************** */
/* MISC Functions                */
/* ***************************** */
static int reset()
{
    return 0;
}

static int writeData(void* _call)
{
    WriterAVCallData_t* call = (WriterAVCallData_t*) _call;

    unsigned char   PesHeader[PES_AUDIO_HEADER_SIZE];

    dts_printf(10, "\n");

    if (call == NULL)
    {
        dts_err("call data is NULL...\n");
        return 0;
    }

    dts_printf(10, "AudioPts %lld\n", call->Pts);

    if ((call->data == NULL) || (call->len <= 0))
    {
        dts_err("parsing NULL Data. ignoring...\n");
        return 0;
    }

    if (call->fd < 0)
    {
        dts_err("file pointer < 0. ignoring ...\n");
        return 0;
    }

// #define DO_BYTESWAP
#ifdef DO_BYTESWAP
    unsigned char *Data = (unsigned char *) malloc(call->len);
    memcpy(Data, call->data, call->len);

    /* 16-bit byte swap all data before injecting it */
    for (i=0; i< call->len; i+=2)
    {
        unsigned char Tmp = Data[i];
        Data[i] = Data[i+1];
        Data[i+1] = Tmp;
    }
#endif

    struct iovec iov[2];

    iov[0].iov_base = PesHeader;
    iov[0].iov_len = InsertPesHeader (PesHeader, call->len, MPEG_AUDIO_PES_START_CODE/*PRIVATE_STREAM_1_PES_START_CODE*/, call->Pts, 0);
#ifdef DO_BYTESPWAP
    iov[1].iov_base = Data;
#else
    iov[1].iov_base = call->data;
#endif
    iov[1].iov_len = call->len;

    int len = writev(call->fd, iov, 2);

#ifdef DO_BYTESWAP
    free(Data);
#endif

    dts_printf(10, "< len %d\n", len);
    return len;
}

/* ***************************** */
/* Writer  Definition            */
/* ***************************** */

static WriterCaps_t caps = {
    "dts",
    eAudio,
    "A_DTS",
    AUDIO_ENCODING_DTS
};

struct Writer_s WriterAudioDTS = {
    &reset,
    &writeData,
    NULL,
    &caps
};
