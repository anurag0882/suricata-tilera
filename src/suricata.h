/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __SURICATA_H__
#define __SURICATA_H__

#include "suricata-common.h"
#include "packet-queue.h"
#include "data-queue.h"

/* the name of our binary */
#define PROG_NAME "Suricata"
#define PROG_VER "1.1beta2"

/* runtime engine control flags */
#define SURICATA_STOP    0x01   /**< gracefully stop the engine: process all
                                     outstanding packets first */
#define SURICATA_KILL    0x02   /**< shut down asap, discarding outstanding
                                     packets. */

/* Engine stage/status*/
enum {
    SURICATA_INIT = 0,
    SURICATA_RUNTIME,
    SURICATA_DEINIT
};

/* Engine is acting as */
enum {
    ENGINE_MODE_IDS,
    ENGINE_MODE_IPS,
};

/** You can use this macros to set/check if we have real drop capabilities */
#define SET_ENGINE_MODE_IPS(engine_mode) do { \
	    (engine_mode) = ENGINE_MODE_IPS; \
    } while (0)
#define SET_ENGINE_MODE_IDS(engine_mode) do { \
	    (engine_mode) = ENGINE_MODE_IDS; \
    } while (0)
#define IS_ENGINE_MODE_IPS(engine_mode)  ((engine_mode) == ENGINE_MODE_IPS)
#define IS_ENGINE_MODE_IDS(engine_mode)  ((engine_mode) == ENGINE_MODE_IDS)

/* queue's between various other threads
 * XXX move to the TmQueue structure later
 */
#ifdef __tile__
PacketQueue trans_q[256] __attribute((aligned(64)));
#else
PacketQueue trans_q[256];
#endif

SCDQDataQueue data_queues[256];
/* memset to zeros, and mutex init! */
void GlobalInits();

extern uint8_t suricata_ctl_flags;

/* uppercase to lowercase conversion lookup table */
uint8_t g_u8_lowercasetable[256];

/* marco to do the actual lookup */
//#define u8_tolower(c) g_u8_lowercasetable[(c)]
// these 2 are slower:
//#define u8_tolower(c) ((c) >= 'A' && (c) <= 'Z') ? g_u8_lowercasetable[(c)] : (c)
//#define u8_tolower(c) (((c) >= 'A' && (c) <= 'Z') ? ((c) + ('a' - 'A')) : (c))

/* this is faster than the table lookup */
#include <ctype.h>
#define u8_tolower(c) tolower((uint8_t)(c))

void EngineStop(void);
void EngineKill(void);

int RunmodeIsUnittests(void);

#endif /* __SURICATA_H__ */

