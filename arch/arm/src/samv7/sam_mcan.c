/****************************************************************************
 * arch/arm/src/samv7/sam_mcan.c
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * References:
 *   SAMV7D3 Series Data Sheet
 *   Atmel sample code
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX, Atmel, nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <semaphore.h>
#include <errno.h>
#include <debug.h>

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/can.h>

#include "cache.h"
#include "up_internal.h"
#include "up_arch.h"

#include "chip/sam_matrix.h"
#include "chip/sam_pinmap.h"
#include "sam_periphclks.h"
#include "sam_gpio.h"
#include "sam_mcan.h"

#if defined(CONFIG_CAN) && defined(CONFIG_SAMV7_MCAN)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
/* Common definitions *******************************************************/

#ifndef MIN
#  define MIN(a,b) ((a < b) ? a : b)
#endif

#ifndef MAX
#  define MAX(a,b) ((a > b) ? a : b)
#endif

/* Clock source *************************************************************/

/* PCK5 is the programmable clock source, common to all MCAN controllers */

#if defined(CONFIG_SAMV7_MCAN_CLKSRC_SLOW)
#  define SAMV7_MCAN_CLKSRC           PMC_PCK_CSS_SLOW
#  define SAMV7_MCAN_CLKSRC_FREQUENCY BOARD_SLOWCLK_FREQUENCY
#elif defined(CONFIG_SAMV7_MCAN_CLKSRC_PLLA)
#  define SAMV7_MCAN_CLKSRC           PMC_PCK_CSS_PLLA
#  define SAMV7_MCAN_CLKSRC_FREQUENCY BOARD_PLLA_FREQUENCY
#elif defined(CONFIG_SAMV7_MCAN_CLKSRC_UPLL)
#  define SAMV7_MCAN_CLKSRC           PMC_PCK_CSS_UPLL
#  define SAMV7_MCAN_CLKSRC_FREQUENCY BOARD_UPLL_FREQUENCY
#elif defined(CONFIG_SAMV7_MCAN_CLKSRC_MCK)
#  define SAMV7_MCAN_CLKSRC           PMC_PCK_CSS_MCK
#  define SAMV7_MCAN_CLKSRC_FREQUENCY BOARD_MCK_FREQUENCY
#else /* if defined(CONFIG_SAMV7_MCAN_CLKSRC_MAIN */
#  define SAMV7_MCAN_CLKSRC           PMC_PCK_CSS_MAIN
#  define SAMV7_MCAN_CLKSRC_FREQUENCY BOARD_MAINOSC_FREQUENCY
#endif

#ifndef CONFIG_SAMV7_MCAN_CLKSRC_PRESCALER
#  define CONFIG_SAMV7_MCAN_CLKSRC_PRESCALER 1
#endif

#define SAMV7_MCANCLK_FREQUENCY \
  (SAMV7_MCAN_CLKSRC_FREQUENCY / CONFIG_SAMV7_MCAN_CLKSRC_PRESCALER)

/* MCAN0 Configuration ******************************************************/

#ifdef CONFIG_SAMV7_MCAN0

/* Bit timing */

#  define MCAN0_TSEG1  (CONFIG_SAMV7_MCAN0_PROPSEG + CONFIG_SAMV7_MCAN0_PHASESEG1)
#  define MCAN0_TSEG2  CONFIG_SAMV7_MCAN0_PHASESEG2
#  define MCAN0_BRP    ((uint32_t)(((float) SAMV7_MCANCLK_FREQUENCY / \
                       ((float)(MCAN0_TSEG1 + MCAN0_TSEG2 + 3) * \
                        (float)CONFIG_SAMV7_MCAN0_BITRATE)) - 1))
#  define MCAN0_SJW    (CONFIG_SAMV7_MCAN0_FSJW - 1)

#  if MCAN0_TSEG1 > 63
#    error Invalid MCAN0 TSEG1
#  endif
#  if MCAN0_TSEG2 > 15
#    error Invalid MCAN0 TSEG2
#  endif
#  if MCAN0_SJW > 15
#    error Invalid MCAN0 SJW
#  endif

#  define MCAN0_FTSEG1 (CONFIG_SAMV7_MCAN0_FPROPSEG + CONFIG_SAMV7_MCAN0_FPHASESEG1)
#  define MCAN0_FTSEG2 (CONFIG_SAMV7_MCAN0_FPHASESEG2)
#  define MCAN0_FBRP   ((uint32_t)(((float) SAMV7_MCANCLK_FREQUENCY / \
                       ((float)(MCAN0_FTSEG1 + MCAN0_FTSEG2 + 3) * \
                        (float)CONFIG_SAMV7_MCAN0_FBITRATE)) - 1))
#  define MCAN0_FSJW   (CONFIG_SAMV7_MCAN0_FFSJW - 1)

#  if MCAN0_FTSEG1 > 15
#    error Invalid MCAN0 FTSEG1
#  endif
#  if MCAN0_FTSEG2 > 7
#    error Invalid MCAN0 FTSEG2
#  endif
#  if MCAN0_FSJW > 3
#    error Invalid MCAN0 FSJW
#  endif

/* MCAN0 RX FIFO0 element size */

#  if defined(CONFIG_SAMV7_MCAN0_RXFIFO0_8BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_12BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_16BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_20BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_24BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_32BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_48BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO0_64BYTES)
#    define MCAN0_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO0_ENCODED_SIZE  7
#  else
#    error Undefined MCAN0 RX FIFO0 element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_RXFIFO0_SIZE
#    define CONFIG_SAMV7_MCAN0_RXFIFO0_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN0_RXFIFO0_SIZE > 64
#    error Invalid MCAN0 number of RX FIFO0 elements
#  endif

#  define MCAN0_RXFIFO0_WORDS \
    (CONFIG_SAMV7_MCAN0_RXFIFO0_SIZE * ((MCAN0_RXFIFO0_ELEMENT_SIZE/4) + 2))

/* MCAN0 RX FIFO1 element size */

#  if defined(CONFIG_SAMV7_MCAN0_RXFIFO1_8BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_12BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_16BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_20BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_24BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_32BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_48BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN0_RXFIFO1_64BYTES)
#    define MCAN0_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN0_RXFIFO1_ENCODED_SIZE  7
#  else
#    error Undefined MCAN0 RX FIFO1 element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_RXFIFO1_SIZE
#    define CONFIG_SAMV7_MCAN0_RXFIFO1_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN0_RXFIFO1_SIZE > 64
#    error Invalid MCAN0 number of RX FIFO1 elements
#  endif

#  define MCAN0_RXFIFO1_WORDS \
    (CONFIG_SAMV7_MCAN0_RXFIFO1_SIZE * ((MCAN1_RXFIFO1_ELEMENT_SIZE/4) + 2))

/* MCAN0 Filters */

#  ifndef CONFIG_SAMV7_MCAN0_NSTDFILTERS
#    define CONFIG_SAMV7_MCAN0_NSTDFILTERS 0
#  endif

#  if (CONFIG_SAMV7_MCAN0_NSTDFILTERS > 128)
#    error Invalid MCAN0 number of Standard Filters
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_NEXTFILTERS
#    define CONFIG_SAMV7_MCAN0_NEXTFILTERS 0
#  endif

#  if (CONFIG_SAMV7_MCAN0_NEXTFILTERS > 64)
#    error Invalid MCAN0 number of Extended Filters
#  endif

#define MCAN0_STDFILTER_WORDS  CONFIG_SAMV7_MCAN0_NSTDFILTERS
#define MCAN0_EXTFILTER_WORDS  (CONFIG_SAMV7_MCAN0_NEXTFILTERS * 2)

/* MCAN0 RX buffer element size */

#  if defined(CONFIG_SAMV7_MCAN0_RXBUFFER_8BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_12BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_16BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_20BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_24BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_32BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_48BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN0_RXBUFFER_64BYTES)
#    define MCAN0_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_RXBUFFER_ENCODED_SIZE  7
#  else
#    error Undefined MCAN0 RX buffer element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE
#    define CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE > 64
#    error Invalid MCAN0 number of RX BUFFER elements
#  endif

#  define MCAN0_DEDICATED_RXBUFFER_WORDS \
    (CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE * \
    ((MCAN0_RXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN0 TX buffer element size */

#  if defined(CONFIG_SAMV7_MCAN0_TXBUFFER_8BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_12BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_16BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_20BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_24BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_32BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_48BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN0_TXBUFFER_64BYTES)
#    define MCAN0_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN0_TXBUFFER_ENCODED_SIZE  7
#  else
#    error Undefined MCAN0 TX buffer element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_DEDICATED_TXBUFFER_SIZE
#    define CONFIG_SAMV7_MCAN0_DEDICATED_TXBUFFER_SIZE 0
#  endif

#  define MCAN0_DEDICATED_TXBUFFER_WORDS \
    (CONFIG_SAMV7_MCAN0_DEDICATED_TXBUFFER_SIZE * \
    ((MCAN0_TXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN0 TX FIFOs */

#  ifndef CONFIG_SAMV7_MCAN0_TXFIFOQ_SIZE
#    define CONFIG_SAMV7_MCAN0_TXFIFOQ_SIZE 0
#  endif

#  if (CONFIG_SAMV7_MCAN0_DEDICATED_TXBUFFER_SIZE + \
       CONFIG_SAMV7_MCAN0_TXFIFOQ_SIZE) > 32
#    error Invalid MCAN0 number of TX BUFFER elements
#  endif

#  ifndef CONFIG_SAMV7_MCAN0_TXEVENTFIFO_SIZE
#    define CONFIG_SAMV7_MCAN0_TXEVENTFIFO_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN0_TXEVENTFIFO_SIZE > 32
#    error Invalid MCAN0 number of TX EVENT FIFO elements
#  endif

#  define MCAN0_TXEVENTFIFO_WORDS  (CONFIG_SAMV7_MCAN0_TXEVENTFIFO_SIZE * 2)
#  define MCAN0_TXFIFIOQ_WORDS \
    (CONFIG_SAMV7_MCAN0_TXFIFOQ_SIZE * ((MCAN0_TXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN0 Message RAM */

#  define MCAN0_STDFILTER_INDEX   0
#  define MCAN0_EXTFILTERS_INDEX  (MCAN0_STDFILTER_INDEX + MCAN0_STDFILTER_WORDS)
#  define MCAN0_RXFIFO0_INDEX     (MCAN0_EXTFILTERS_INDEX + MCAN0_EXTFILTER_WORDS)
#  define MCAN0_RXFIFO1_INDEX     (MCAN0_RXFIFO0_INDEX + MCAN0_RXFIFO0_WORDS)
#  define MCAN0_RXDEDICATED_INDEX (MCAN0_RXFIFO1_INDEX + MCAN0_RXFIFO1_WORDS)
#  define MCAN0_TXEVENTFIFO_INDEX (MCAN0_RXDEDICATED_INDEX + MCAN0_DEDICATED_RXBUFFER_WORDS)
#  define MCAN0_TXDEDICATED_INDEX (MCAN0_TXEVENTFIFO_INDEX + MCAN0_TXEVENTFIFO_WORDS)
#  define MCAN0_TXFIFOQ_INDEX     (MCAN0_TXDEDICATED_INDEX + MCAN0_DEDICATED_TXBUFFER_WORDS)
#  define MCAN0_MSGRAM_WORDS      (MCAN0_TXFIFOQ_INDEX + MCAN0_TXFIFIOQ_WORDS)

#endif /* CONFIG_SAMV7_MCAN0 */

/* Loopback mode */

#undef SAMV7_MCAN_LOOPBACK
#if defined(CONFIG_SAMV7_MCAN0_LOOPBACK) || defined(CONFIG_SAMV7_MCAN1_LOOPBACK)
#  define SAMV7_MCAN_LOOPBACK 1
#endif

/* MCAN1 Configuration ******************************************************/

#ifdef CONFIG_SAMV7_MCAN1
  /* Bit timing */

#  define MCAN1_TSEG1  (CONFIG_SAMV7_MCAN1_PROPSEG + CONFIG_SAMV7_MCAN1_PHASESEG1)
#  define MCAN1_TSEG2  CONFIG_SAMV7_MCAN1_PHASESEG2
#  define MCAN1_BRP    ((uint32_t)(((float) SAMV7_MCANCLK_FREQUENCY / \
                       ((float)(MCAN1_TSEG1 + MCAN1_TSEG2 + 3) * \
                        (float)CONFIG_SAMV7_MCAN1_BITRATE)) - 1))
#  define MCAN1_SJW    (CONFIG_SAMV7_MCAN1_FSJW - 1)

#  if MCAN1_TSEG1 > 63
#    error Invalid MCAN1 TSEG1
#  endif
#  if MCAN1_TSEG2 > 15
#    error Invalid MCAN1 TSEG2
#  endif
#  if MCAN1_SJW > 15
#    error Invalid MCAN1 SJW
#  endif

#  define MCAN1_FTSEG1 (CONFIG_SAMV7_MCAN1_FPROPSEG + CONFIG_SAMV7_MCAN1_FPHASESEG1)
#  define MCAN1_FTSEG2 (CONFIG_SAMV7_MCAN1_FPHASESEG2)
#  define MCAN1_FBRP   ((uint32_t)(((float) SAMV7_MCANCLK_FREQUENCY / \
                       ((float)(MCAN1_FTSEG1 + MCAN1_FTSEG2 + 3) * \
                        (float)CONFIG_SAMV7_MCAN1_FBITRATE)) - 1))
#  define MCAN1_FSJW   (CONFIG_SAMV7_MCAN1_FFSJW - 1)

#if MCAN1_FTSEG1 > 15
#  error Invalid MCAN1 FTSEG1
#endif
#if MCAN1_FTSEG2 > 7
#  error Invalid MCAN1 FTSEG2
#endif
#if MCAN1_FSJW > 3
#  error Invalid MCAN1 FSJW
#endif

/* MCAN1 RX FIFO0 element size */

#  if defined(CONFIG_SAMV7_MCAN1_RXFIFO0_8BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_12BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_16BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_20BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_24BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_32BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_48BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO0_64BYTES)
#    define MCAN1_RXFIFO0_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO0_ENCODED_SIZE  7
#  else
#    error Undefined MCAN1 RX FIFO0 element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_RXFIFO0_SIZE
#    define CONFIG_SAMV7_MCAN1_RXFIFO0_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN1_RXFIFO0_SIZE > 64
#    error Invalid MCAN1 number of RX FIFO 0 elements
#  endif

#  define MCAN1_RXFIFO0_WORDS \
     (CONFIG_SAMV7_MCAN1_RXFIFO0_SIZE * ((MCAN1_RXFIFO0_ELEMENT_SIZE/4) + 2))

/* MCAN1 RX FIFO1 element size */

#  if defined(CONFIG_SAMV7_MCAN1_RXFIFO1_8BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_12BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_16BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_20BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_24BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_32BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_48BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN1_RXFIFO1_64BYTES)
#    define MCAN1_RXFIFO1_ELEMENT_SIZE  8
#    define MCAN1_RXFIFO1_ENCODED_SIZE  7
#  else
#    error Undefined MCAN1 RX FIFO1 element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_RXFIFO1_SIZE
#    define CONFIG_SAMV7_MCAN1_RXFIFO1_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN1_RXFIFO1_SIZE > 64
#    error Invalid MCAN1 number of RX FIFO 0 elements
#  endif

#  define MCAN1_RXFIFO1_WORDS \
     (CONFIG_SAMV7_MCAN1_RXFIFO1_SIZE * ((MCAN1_RXFIFO1_ELEMENT_SIZE/4) + 2))

/* MCAN1 Filters */

#  ifndef CONFIG_SAMV7_MCAN1_NSTDFILTERS
#    define CONFIG_SAMV7_MCAN1_NSTDFILTERS 0
#  endif

#  if CONFIG_SAMV7_MCAN1_NSTDFILTERS > 128
#    error Invalid MCAN1 number of Standard Filters
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_NEXTFILTERS
#    define CONFIG_SAMV7_MCAN1_NEXTFILTERS 0
#  endif

#  if CONFIG_SAMV7_MCAN1_NEXTFILTERS > 64
#    error Invalid MCAN1 number of Extended Filters
#  endif

#  define MCAN1_STDFILTER_WORDS  CONFIG_SAMV7_MCAN1_NSTDFILTERS
#  define MCAN1_EXTFILTER_WORDS (CONFIG_SAMV7_MCAN1_NEXTFILTERS * 2)

/* MCAN1 RX buffer element size */

#  if defined(CONFIG_SAMV7_MCAN1_RXBUFFER_8BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_12BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_16BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_20BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_24BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_32BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_48BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN1_RXBUFFER_64BYTES)
#    define MCAN1_RXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_RXBUFFER_ENCODED_SIZE  7
#  else
#    error Undefined MCAN1 RX buffer element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_DEDICATED_RXBUFFER_SIZE
#    define CONFIG_SAMV7_MCAN1_DEDICATED_RXBUFFER_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN1_DEDICATED_RXBUFFER_SIZE > 64
#    error Invalid MCAN1 number of RX BUFFER elements
#  endif

#  define MCAN1_DEDICATED_RXBUFFER_WORDS \
     (CONFIG_SAMV7_MCAN1_DEDICATED_RXBUFFER_SIZE * \
     ((MCAN1_RXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN1 TX buffer element size */

#  if defined(CONFIG_SAMV7_MCAN1_TXBUFFER_8BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  0
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_12BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  1
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_16BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  2
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_20BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  3
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_24BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  4
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_32BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  5
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_48BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  6
#  elif defined(CONFIG_SAMV7_MCAN1_TXBUFFER_64BYTES)
#    define MCAN1_TXBUFFER_ELEMENT_SIZE  8
#    define MCAN1_TXBUFFER_ENCODED_SIZE  7
#  else
#    error Undefined MCAN1 TX buffer element size
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_DEDICATED_TXBUFFER_SIZE
#    define CONFIG_SAMV7_MCAN1_DEDICATED_TXBUFFER_SIZE 0
#  endif

#  define MCAN1_DEDICATED_TXBUFFER_WORDS \
     (CONFIG_SAMV7_MCAN1_DEDICATED_TXBUFFER_SIZE * \
     ((MCAN1_TXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN1 TX FIFOs */

#  ifndef CONFIG_SAMV7_MCAN1_TXFIFOQ_SIZE
#    define CONFIG_SAMV7_MCAN1_TXFIFOQ_SIZE 0
#  endif

#  if (CONFIG_SAMV7_MCAN1_DEDICATED_TXBUFFER_SIZE + \
       CONFIG_SAMV7_MCAN1_TXFIFOQ_SIZE) > 32
#    error Invalid MCAN1 number of TX BUFFER elements
#  endif

#  ifndef CONFIG_SAMV7_MCAN1_TXEVENTFIFO_SIZE
#    define CONFIG_SAMV7_MCAN1_TXEVENTFIFO_SIZE 0
#  endif

#  if CONFIG_SAMV7_MCAN1_TXEVENTFIFO_SIZE > 32
#    error Invalid MCAN1 number of TX EVENT FIFO elements
#  endif

#  define MCAN1_TXEVENTFIFO_WORDS (CONFIG_SAMV7_MCAN1_TXEVENTFIFO_SIZE * 2)
#  define MCAN1_TXFIFIOQ_WORDS \
     (CONFIG_SAMV7_MCAN1_TXFIFOQ_SIZE * ((MCAN0_TXBUFFER_ELEMENT_SIZE/4) + 2))

/* MCAN1 Message RAM */

#  define MCAN1_STDFILTER_INDEX   0
#  define MCAN1_EXTFILTERS_INDEX  (MCAN1_STDFILTER_INDEX + MCAN1_STDFILTER_WORDS)
#  define MCAN1_RXFIFO0_INDEX     (MCAN1_EXTFILTERS_INDEX + MCAN1_EXTFILTER_WORDS)
#  define MCAN1_RXFIFO1_INDEX     (MCAN1_RXFIFO0_INDEX + MCAN1_RXFIFO0_WORDS)
#  define MCAN1_RXDEDICATED_INDEX (MCAN1_RXFIFO1_INDEX + MCAN1_RXFIFO1_WORDS)
#  define MCAN1_TXEVENTFIFO_INDEX (MCAN1_RXDEDICATED_INDEX + MCAN1_DEDICATED_RXBUFFER_WORDS)
#  define MCAN1_TXDEDICATED_INDEX (MCAN1_TXEVENTFIFO_INDEX + MCAN1_TXEVENTFIFO_WORDS)
#  define MCAN1_TXFIFOQ_INDEX     (MCAN1_TXDEDICATED_INDEX + MCAN1_DEDICATED_TXBUFFER_WORDS)
#  define MCAN1_MSGRAM_WORDS      (MCAN1_TXFIFOQ_INDEX + MCAN1_TXFIFIOQ_WORDS)

#endif /* CONFIG_SAMV7_MCAN1 */

/* MCAN helpers *************************************************************/

#define MAILBOX_ADDRESS(a)        ((uint32_t)(a) & 0x0000fffc)

/* Interrupts ***************************************************************/
/* Common interrupts
 *
 *   MCAN_INT_TSW  - Timestamp Wraparound
 *   MCAN_INT_MRAF - Message RAM Access Failure
 *   MCAN_INT_TOO  - Timeout Occurred
 *   MCAN_INT_ELO  - Error Logging Overflow
 *   MCAN_INT_EP   - Error Passive
 *   MCAN_INT_EW   - Warning Status
 *   MCAN_INT_BO   - Bus_Off Status
 *   MCAN_INT_WDI  - Watchdog Interrupt
 */

#define MCAN_CMNERR_INTS   (MCAN_INT_MRAF | MCAN_INT_TOO | MCAN_INT_EP | \
                            MCAN_INT_BO | MCAN_INT_WDI)
#define MCAN_COMMON_INTS   MCAN_CMNERR_INTS

/* RXFIFO mode interrupts
 *
 *   MCAN_INT_RF0N - Receive FIFO 0 New Message
 *   MCAN_INT_RF0W - Receive FIFO 0 Watermark Reached
 *   MCAN_INT_RF0F - Receive FIFO 0 Full
 *   MCAN_INT_RF0L - Receive FIFO 0 Message Lost
 *   MCAN_INT_RF1N - Receive FIFO 1 New Message
 *   MCAN_INT_RF1W - Receive FIFO 1 Watermark Reached
 *   MCAN_INT_RF1F - Receive FIFO 1 Full
 *   MCAN_INT_RF1L - Receive FIFO 1 Message Lost
 *   MCAN_INT_HPM  - High Priority Message Received
 *
 * Dedicated RX Buffer mode interrupts
 *
 *   MCAN_INT_DRX  - Message stored to Dedicated Receive Buffer
 *
 * Mode-independent RX-related interrupts
 *
 *   MCAN_INT_CRCE - Receive CRC Error
 *   MCAN_INT_FOE  - Format Error
 *   MCAN_INT_STE  - Stuff Error
 */

#define MCAN_RXCOMMON_INTS (MCAN_INT_CRCE | MCAN_INT_FOE | MCAN_INT_STE)
#define MCAN_RXFIFO0_INTS  (MCAN_INT_RF0N | MCAN_INT_RF0W | MCAN_INT_RF0L | \
                            MCAN_INT_RF0L)
#define MCAN_RXFIFO1_INTS  (MCAN_INT_RF1N | MCAN_INT_RF1W | MCAN_INT_RF1L | \
                            MCAN_INT_RF1L)
#define MCAN_RXFIFO_INTS   (MCAN_RXFIFO0_INTS | MCAN_RXFIFO1_INTS | \
                            MCAN_INT_HPM | MCAN_RXCOMMON_INTS)
#define MCAN_RXDEDBUF_INTS (MCAN_INT_DRX | MCAN_RXCOMMON_INTS)

#define MCAN_RXERR_INTS    (MCAN_INT_RF0L | MCAN_INT_RF1L | MCAN_INT_CRCE | \
                            MCAN_INT_FOE | MCAN_INT_STE)

/* TX FIFOQ mode interrupts
 *
 *   MCAN_INT_TFE  - Tx FIFO Empty
 *
 * TX Event FIFO interrupts
 *
 *   MCAN_INT_TEFN - Tx Event FIFO New Entry
 *   MCAN_INT_TEFW - Tx Event FIFO Watermark Reached
 *   MCAN_INT_TEFF - Tx Event FIFO Full
 *   MCAN_INT_TEFL - Tx Event FIFO Element Lost
 *
 * Mode-independent TX-related interrupts
 *
 *   MCAN_INT_TC   - Transmission Completed
 *   MCAN_INT_TCF  - Transmission Cancellation Finished
 *   MCAN_INT_BE   - Bit Error
 *   MCAN_INT_ACKE - Acknowledge Error
 */

#define MCAN_TXCOMMON_INTS (MCAN_INT_TC | MCAN_INT_TCF | MCAN_INT_BE | \
                            MCAN_INT_ACKE)
#define MCAN_TXFIFOQ_INTS  (MCAN_INT_TFE | MCAN_TXCOMMON_INTS)
#define MCAN_TXEVFIFO_INTS (MCAN_INT_TEFN | MCAN_INT_TEFW | MCAN_INT_TEFF | \
                            MCAN_INT_TEFL)
#define MCAN_TXDEDBUG_INTS MCAN_TXCOMMON_INTS

#define MCAN_TXERR_INTS    (MCAN_INT_TEFL | MCAN_INT_BE | MCAN_INT_ACKE)

/* Debug ********************************************************************/
/* Non-standard debug that may be enabled just for testing MCAN */

#ifdef CONFIG_DEBUG_CAN
#  define candbg    dbg
#  define canvdbg   vdbg
#  define canlldbg  lldbg
#  define canllvdbg llvdbg
#else
#  define candbg(x...)
#  define canvdbg(x...)
#  define canlldbg(x...)
#  define canllvdbg(x...)
#endif

#if !defined(CONFIG_DEBUG) || !defined(CONFIG_DEBUG_CAN)
#  undef CONFIG_SAMV7_MCAN_REGDEBUG
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/
/* CAN mode of operation */

enum sam_canmode_e
{
  MCAN_ISO11898_1_MODE = 0, /* CAN operation according to ISO11898-1 */
  MCAN_FD_MODE         = 1, /* CAN FD operation */
  MCAN_FD_BSW_MODE     = 2  /* CAN FD operation with bit rate switching */
};

/* This structure describes the MCAN message RAM layout */

struct sam_msgram_s
{
  uint32_t *stdfilters;    /* Standard filters */
  uint32_t *extfilters;    /* Extended filters */
  uint32_t *rxfifo0;       /* RX FIFO0 */
  uint32_t *rxfifo1;       /* RX FIFO1 */
  uint32_t *rxdedicated;   /* RX dedicated buffers */
  uint32_t *txeventfifo;   /* TX event FIFO */
  uint32_t *txdedicated;   /* TX dedicated buffers */
  uint32_t *txfifoq;       /* TX FIFO queue */
};

/* This structure provides the constant configuration of a MCAN peripheral */

struct sam_config_s
{
  gpio_pinset_t rxpinset;   /* RX pin configuration */
  gpio_pinset_t txpinset;   /* TX pin configuration */
  xcpt_t handler;           /* MCAN common interrupt handler */
  uintptr_t base;           /* Base address of the MCAN registers */
  uint32_t baud;            /* Configured baud */
  uint32_t btp;             /* Bit timing/prescaler register setting */
  uint32_t fbtp;            /* Fast bit timing/prescaler register setting */
  uint8_t port;             /* MCAN port number (1 or 2) */
  uint8_t pid;              /* MCAN peripheral ID */
  uint8_t irq0;             /* MCAN peripheral IRQ number for interrupt line 0 */
  uint8_t irq1;             /* MCAN peripheral IRQ number for interrupt line 1 */
  uint8_t mode;             /* See enum sam_canmode_e */
  uint8_t nstdfilters;      /* Number of standard filters (up to 128) */
  uint8_t nextfilters;      /* Number of extended filters (up to 64) */
  uint8_t nfifo0;           /* Number of FIFO0 elements (up to 64) */
  uint8_t nfifo1;           /* Number of FIFO1 elements (up to 64) */
  uint8_t nrxdedicated;     /* Number of dedicated RX buffers (up to 64) */
  uint8_t ntxeventfifo;     /* Number of TXevent FIFO elements (up to 32) */
  uint8_t ntxdedicated;     /* Number of dedicated TX buffers (up to 64) */
  uint8_t ntxfifoq;         /* Number of TX FIFO queue elements (up to 32) */
  uint8_t rxfifo0ecode;     /* Encoded RX FIFO0 element size */
  uint8_t rxfifo0esize;     /* RX FIFO0 element size */
  uint8_t rxfifo1ecode;     /* Encoded RX FIFO1 element size */
  uint8_t rxfifo1esize;     /* RX FIFO1 element size */
  uint8_t rxbufferecode;    /* Encoded RX buffer element size */
  uint8_t rxbufferesize;    /* RX buffer element size */
  uint8_t txbufferecode;    /* Encoded TX buffer element size */
  uint8_t txbufferesize;    /* TX buffer element size */
#ifdef SAMV7_MCAN_LOOPBACK
  bool loopback;            /* True: Loopback mode */
#endif

  /* MCAN message RAM layout */

  struct sam_msgram_s msgram;
};

/* This structure provides the current state of a MCAN peripheral */

struct sam_mcan_s
{
  const struct sam_config_s *config; /* The constant configuration */
  bool initialized;         /* True: Device has been initialized */
  bool  txenabled;          /* True: TX interrupts have been enabled */
  sem_t locksem;            /* Enforces mutually exclusive access */
  sem_t txfsem;             /* Used to wait for TX FIFO availability */
  uint32_t rxints;          /* Configured RX interrupts */
  uint32_t txints;          /* Configured TX interrupts */

#ifdef CONFIG_SAMV7_MCAN_REGDEBUG
  uintptr_t regaddr;        /* Last register address read */
  uint32_t regval;          /* Last value read from the register */
  unsigned int count;       /* Number of times that the value was read */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* MCAN Register access */

static uint32_t mcan_getreg(FAR struct sam_mcan_s *priv, int offset);
static void mcan_putreg(FAR struct sam_mcan_s *priv, int offset,
              uint32_t regval);
#ifdef CONFIG_SAMV7_MCAN_REGDEBUG
static void mcan_dumpregs(FAR struct sam_mcan_s *priv, FAR const char *msg);
#else
#  define mcan_dumpregs(priv,msg)
#endif

/* Semaphore helpers */

static void mcan_dev_lock(FAR struct sam_mcan_s *priv);
#define mcan_dev_unlock(priv) sem_post(&priv->locksem)

static void mcan_buffer_reserve(FAR struct sam_mcan_s *priv);
#define mcan_buffer_release(priv) sem_post(&priv->txfsem)

/* Mailboxes */

static int  mcan_recvsetup(FAR struct sam_mcan_s *priv);

/* CAN driver methods */

static void mcan_reset(FAR struct can_dev_s *dev);
static int  mcan_setup(FAR struct can_dev_s *dev);
static void mcan_shutdown(FAR struct can_dev_s *dev);
static void mcan_rxint(FAR struct can_dev_s *dev, bool enable);
static void mcan_txint(FAR struct can_dev_s *dev, bool enable);
static int  mcan_ioctl(FAR struct can_dev_s *dev, int cmd,
              unsigned long arg);
static int  mcan_remoterequest(FAR struct can_dev_s *dev, uint16_t id);
static int  mcan_send(FAR struct can_dev_s *dev, FAR struct can_msg_s *msg);
static bool mcan_txready(FAR struct can_dev_s *dev);
static bool mcan_txempty(FAR struct can_dev_s *dev);

/* MCAN interrupt handling */

#if 0 /* Not Used */
static bool mcan_dedicated_rxbuffer_available(FAR struct sam_mcan_s *priv,
              int bufndx);
#endif
static void mcan_receive(FAR struct can_dev_s *dev,
              FAR uint32_t *rxbuffer, unsigned long nbytes);
static void mcan_interrupt(FAR struct can_dev_s *dev);
#ifdef CONFIG_SAMV7_MCAN0
static int  mcan0_interrupt(int irq, void *context);
#endif
#ifdef CONFIG_SAMV7_MCAN1
static int  mcan1_interrupt(int irq, void *context);
#endif

/* Hardware initialization */

static void mcan_configure_interrupts(FAR struct sam_mcan_s *priv);
static int  mcan_hw_initialize(FAR struct sam_mcan_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct can_ops_s g_mcanops =
{
  .co_reset         = mcan_reset,
  .co_setup         = mcan_setup,
  .co_shutdown      = mcan_shutdown,
  .co_rxint         = mcan_rxint,
  .co_txint         = mcan_txint,
  .co_ioctl         = mcan_ioctl,
  .co_remoterequest = mcan_remoterequest,
  .co_send          = mcan_send,
  .co_txready       = mcan_txready,
  .co_txempty       = mcan_txempty,
};

#ifdef CONFIG_SAMV7_MCAN0
/* Message RAM allocation */

static uint32_t g_mcan0_msgram[MCAN0_MSGRAM_WORDS];

/* Constant configuration */

static const struct sam_config_s g_mcan0const =
{
  .rxpinset         = GPIO_MCAN0_RX,
  .txpinset         = GPIO_MCAN0_TX,
  .handler          = mcan0_interrupt,
  .base             = SAM_MCAN0_BASE,
  .baud             = CONFIG_SAMV7_MCAN0_BITRATE,
  .btp              = MCAN_BTP_BRP(MCAN0_BRP) | MCAN_BTP_TSEG1(MCAN0_TSEG1) |
                      MCAN_BTP_TSEG2(MCAN0_TSEG2) | MCAN_BTP_SJW(MCAN0_SJW),
  .fbtp             = MCAN_FBTP_FBRP(MCAN0_FBRP) | MCAN_FBTP_FTSEG1(MCAN0_FTSEG1) |
                      MCAN_FBTP_FTSEG2(MCAN0_FTSEG2) | MCAN_FBTP_FSJW(MCAN0_FSJW),
  .port             = 0,
  .pid              = SAM_PID_MCAN00,
  .irq0             = SAM_IRQ_MCAN01,
#if defined(CONFIG_SAMV7_MCAN0_ISO11899_1)
  .mode             = MCAN_ISO11898_1_MODE,
#elif defined(CONFIG_SAMV7_MCAN0_FD)
  .mode             = MCAN_FD_MODE,
#else /* if defined(CONFIG_SAMV7_MCAN0_FD_BSW) */
  .mode             = MCAN_FD_BSW_MODE,
#endif
  .nstdfilters      = CONFIG_SAMV7_MCAN0_NSTDFILTERS,
  .nextfilters      = CONFIG_SAMV7_MCAN0_NEXTFILTERS,
  .nfifo0           = CONFIG_SAMV7_MCAN0_RXFIFO0_SIZE,
  .nfifo1           = CONFIG_SAMV7_MCAN0_RXFIFO1_SIZE,
  .nrxdedicated     = CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE,
  .ntxeventfifo     = CONFIG_SAMV7_MCAN0_TXEVENTFIFO_SIZE,
  .ntxdedicated     = CONFIG_SAMV7_MCAN0_DEDICATED_TXBUFFER_SIZE,
  .ntxfifoq         = CONFIG_SAMV7_MCAN0_TXFIFOQ_SIZE,
  .rxfifo0ecode     = MCAN0_RXFIFO0_ENCODED_SIZE,
  .rxfifo0esize     = (MCAN0_RXFIFO0_ELEMENT_SIZE / 4) + 2,
  .rxfifo1ecode     = MCAN0_RXFIFO1_ENCODED_SIZE,
  .rxfifo1esize     = (MCAN0_RXFIFO1_ELEMENT_SIZE / 4) + 2,
  .rxbufferecode    = MCAN0_RXBUFFER_ENCODED_SIZE,
  .rxbufferesize    = (MCAN0_RXBUFFER_ELEMENT_SIZE / 4) + 2,
  .txbufferecode    = MCAN0_TXBUFFER_ENCODED_SIZE,
  .txbufferesize    = (MCAN0_TXBUFFER_ELEMENT_SIZE / 4) + 2,

#ifdef CONFIG_SAMV7_MCAN0_LOOPBACK
  .loopback         = true,
#endif

  /* MCAN0 Message RAM */

  .msgram =
  {
     &g_mcan0_msgram[MCAN0_STDFILTER_INDEX],
     &g_mcan0_msgram[MCAN0_EXTFILTERS_INDEX],
     &g_mcan0_msgram[MCAN0_RXFIFO0_INDEX],
     &g_mcan0_msgram[MCAN0_RXFIFO1_INDEX],
     &g_mcan0_msgram[MCAN0_RXDEDICATED_INDEX],
     &g_mcan0_msgram[MCAN0_TXEVENTFIFO_INDEX],
     &g_mcan0_msgram[MCAN0_TXDEDICATED_INDEX],
     &g_mcan0_msgram[MCAN0_TXFIFOQ_INDEX]
  }
};

/* MCAN0 variable driver state */

static struct sam_mcan_s g_mcan0priv;
static struct can_dev_s g_mcan0dev;

#endif /* CONFIG_SAMV7_MCAN0 */

#ifdef CONFIG_SAMV7_MCAN1
/* MCAN1 message RAM allocation */

static uint32_t g_mcan1_msgram[MCAN1_MSGRAM_WORDS];

/* MCAN1 constant configuration */

static const struct sam_config_s g_mcan1const =
{
  .rxpinset         = GPIO_MCAN1_RX,
  .txpinset         = GPIO_MCAN1_TX,
  .handler          = mcan1_interrupt,
  .base             = SAM_MCAN1_BASE,
  .baud             = CONFIG_SAMV7_MCAN1_BITRATE,
  .btp              = MCAN_BTP_BRP(MCAN1_BRP) | MCAN_BTP_TSEG1(MCAN1_TSEG1) |
                      MCAN_BTP_TSEG2(MCAN1_TSEG2) | MCAN_BTP_SJW(MCAN1_SJW),
  .fbtp             = MCAN_FBTP_FBRP(MCAN1_FBRP) | MCAN_FBTP_FTSEG1(MCAN1_FTSEG1) |
                      MCAN_FBTP_FTSEG2(MCAN1_FTSEG2) | MCAN_FBTP_FSJW(MCAN1_FSJW),
  .port             = 1,
  .pid              = SAM_PID_MCAN10,
  .irq0             = SAM_IRQ_MCAN11,
#if defined(CONFIG_SAMV7_MCAN1_ISO11899_1)
  .mode             = MCAN_ISO11898_1_MODE,
#elif defined(CONFIG_SAMV7_MCAN1_FD)
  .mode             = MCAN_FD_MODE,
#else /* if defined(CONFIG_SAMV7_MCAN1_FD_BSW) */
  .mode             = MCAN_FD_BSW_MODE,
#endif
  .nstdfilters      = CONFIG_SAMV7_MCAN1_NSTDFILTERS,
  .nextfilters      = CONFIG_SAMV7_MCAN1_NEXTFILTERS,
  .nfifo0           = CONFIG_SAMV7_MCAN1_RXFIFO0_SIZE,
  .nfifo1           = CONFIG_SAMV7_MCAN1_RXFIFO1_SIZE,
  .nrxdedicated     = CONFIG_SAMV7_MCAN0_DEDICATED_RXBUFFER_SIZE,
  .ntxeventfifo     = CONFIG_SAMV7_MCAN1_TXEVENTFIFO_SIZE,
  .ntxdedicated     = CONFIG_SAMV7_MCAN1_DEDICATED_TXBUFFER_SIZE,
  .ntxfifoq         = CONFIG_SAMV7_MCAN1_TXFIFOQ_SIZE,
  .rxfifo0ecode     = MCAN1_RXFIFO0_ENCODED_SIZE,
  .rxfifo0esize     = (MCAN1_RXFIFO0_ELEMENT_SIZE / 4) + 2,
  .rxfifo1ecode     = MCAN1_RXFIFO1_ENCODED_SIZE,
  .rxfifo1esize     = (MCAN1_RXFIFO1_ELEMENT_SIZE / 4) + 2,
  .rxbufferecode    = MCAN1_RXBUFFER_ENCODED_SIZE,
  .rxbufferesize    = (MCAN1_RXBUFFER_ELEMENT_SIZE / 4) + 2,
  .txbufferecode    = MCAN1_TXBUFFER_ENCODED_SIZE,
  .txbufferesize    = (MCAN0_TXBUFFER_ELEMENT_SIZE / 4) + 2,

#ifdef CONFIG_SAMV7_MCAN1_LOOPBACK
  .loopback         = true,
#endif
  /* MCAN0 Message RAM */

  .msgram =
  {
     &g_mcan1_msgram[MCAN1_STDFILTER_INDEX],
     &g_mcan1_msgram[MCAN1_EXTFILTERS_INDEX],
     &g_mcan1_msgram[MCAN1_RXFIFO0_INDEX],
     &g_mcan1_msgram[MCAN1_RXFIFO1_INDEX],
     &g_mcan1_msgram[MCAN1_RXDEDICATED_INDEX],
     &g_mcan1_msgram[MCAN1_TXEVENTFIFO_INDEX],
     &g_mcan1_msgram[MCAN1_TXDEDICATED_INDEX],
     &g_mcan1_msgram[MCAN1_TXFIFOQ_INDEX]
  }
};

/* MCAN0 variable driver state */

static struct sam_mcan_s g_mcan1priv;
static struct can_dev_s g_mcan1dev;

#endif /* CONFIG_SAMV7_MCAN1 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mcan_getreg
 *
 * Description:
 *   Read the value of a MCAN register.
 *
 * Input Parameters:
 *   priv - A reference to the MCAN peripheral state
 *   offset - The offset to the register to read
 *
 * Returned Value:
 *
 ****************************************************************************/

#ifdef CONFIG_SAMV7_MCAN_REGDEBUG
static uint32_t mcan_getreg(FAR struct sam_mcan_s *priv, int offset)
{
  FAR const struct sam_config_s *config = priv->config;
  uintptr_t regaddr;
  uint32_t regval;

  /* Read the value from the register */

  regaddr = config->base + offset;
  regval  = getreg32(regaddr);

  /* Is this the same value that we read from the same register last time?
   * Are we polling the register?  If so, suppress some of the output.
   */

  if (regaddr == priv->regaddr && regval == priv->regval)
    {
      if (priv->count == 0xffffffff || ++priv->count > 3)
        {
           if (priv->count == 4)
             {
               lldbg("...\n");
             }

          return regval;
        }
    }

  /* No this is a new address or value */

  else
    {
       /* Did we print "..." for the previous value? */

       if (priv->count > 3)
         {
           /* Yes.. then show how many times the value repeated */

           lldbg("[repeats %d more times]\n", priv->count - 3);
         }

       /* Save the new address, value, and count */

       priv->regaddr = regaddr;
       priv->regval  = regval;
       priv->count   = 1;
    }

  /* Show the register value read */

  lldbg("%08x->%08x\n", regaddr, regval);
  return regval;
}

#else
static uint32_t mcan_getreg(FAR struct sam_mcan_s *priv, int offset)
{
  FAR const struct sam_config_s *config = priv->config;
  return getreg32(config->base + offset);
}

#endif

/****************************************************************************
 * Name: mcan_putreg
 *
 * Description:
 *   Set the value of a MCAN register.
 *
 * Input Parameters:
 *   priv - A reference to the MCAN peripheral state
 *   offset - The offset to the register to write
 *   regval - The value to write to the register
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_SAMV7_MCAN_REGDEBUG
static void mcan_putreg(FAR struct sam_mcan_s *priv, int offset, uint32_t regval)
{
  FAR const struct sam_config_s *config = priv->config;
  uintptr_t regaddr = config->base + offset;

  /* Show the register value being written */

  lldbg("%08x<-%08x\n", regaddr, regval);

  /* Write the value */

  putreg32(regval, regaddr);
}

#else
static void mcan_putreg(FAR struct sam_mcan_s *priv, int offset, uint32_t regval)
{
  FAR const struct sam_config_s *config = priv->config;
  putreg32(regval, config->base + offset);
}

#endif

/****************************************************************************
 * Name: mcan_dumpregs
 *
 * Description:
 *   Dump the contents of all MCAN control registers
 *
 * Input Parameters:
 *   priv - A reference to the MCAN peripheral state
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

#ifdef CONFIG_SAMV7_MCAN_REGDEBUG
static void mcan_dumpregs(FAR struct sam_mcan_s *priv, FAR const char *msg)
{
  FAR const struct sam_config_s *config = priv->config;
  unsigned long addr;

  lldbg("MCAN%d Registers: %s\n", config->port, msg);
  lldbg("   Base: %08x\n", config->base);

  lldbg("   CUST: %08x  FBTP: %08x TEST: %08x    RWD: %08x\n",
        getreg32(config->base + SAM_MCAN_CUST_OFFSET),
        getreg32(config->base + SAM_MCAN_FBTP_OFFSET),
        getreg32(config->base + SAM_MCAN_TEST_OFFSET),
        getreg32(config->base + SAM_MCAN_RWD_OFFSET));

  lldbg("  CCCR: %08x   BTP: %08x  TSCC: %08x   TSCV: %08x\n",
        getreg32(config->base + SAM_MCAN_CCCR_OFFSET),
        getreg32(config->base + SAM_MCAN_BTP_OFFSET),
        getreg32(config->base + SAM_MCAN_TSCC_OFFSET),
        getreg32(config->base + SAM_MCAN_TSCV_OFFSET));

  lldbg("  TOCC: %08x  TOCV: %08x   ECR: %08x    PSR: %08x\n",
        getreg32(config->base + SAM_MCAN_TOCC_OFFSET),
        getreg32(config->base + SAM_MCAN_TOCV_OFFSET),
        getreg32(config->base + SAM_MCAN_ECR_OFFSET),
        getreg32(config->base + SAM_MCAN_PSR_OFFSET));

  lldbg("    IR: %08x    IE: %08x   ILS: %08x    ILE: %08x\n",
        getreg32(config->base + SAM_MCAN_IR_OFFSET),
        getreg32(config->base + SAM_MCAN_IE_OFFSET),
        getreg32(config->base + SAM_MCAN_ILS_OFFSET),
        getreg32(config->base + SAM_MCAN_ILE_OFFSET));

  lldbg("   GFC: %08x SIDFC: %08x XIDFC: %08x  XIDAM: %08x\n",
        getreg32(config->base + SAM_MCAN_GFC_OFFSET),
        getreg32(config->base + SAM_MCAN_SIDFC_OFFSET),
        getreg32(config->base + SAM_MCAN_XIDFC_OFFSET),
        getreg32(config->base + SAM_MCAN_XIDAM_OFFSET));

  lldbg("  HPMS: %08x NDAT1: %08x NDAT2: %08x  RXF0C: %08x\n",
        getreg32(config->base + SAM_MCAN_HPMS_OFFSET),
        getreg32(config->base + SAM_MCAN_NDAT1_OFFSET),
        getreg32(config->base + SAM_MCAN_NDAT2_OFFSET),
        getreg32(config->base + SAM_MCAN_RXF0C_OFFSET));

  lldbg(" RXF0S: %08x FXF0A: %08x  RXBC: %08x  RXF1C: %08x\n",
        getreg32(config->base + SAM_MCAN_RXF0S_OFFSET),
        getreg32(config->base + SAM_MCAN_RXF0A_OFFSET),
        getreg32(config->base + SAM_MCAN_RXBC_OFFSET),
        getreg32(config->base + SAM_MCAN_RXF1C_OFFSET));

  lldbg(" RXF1S: %08x FXF1A: %08x RXESC: %08x   TXBC: %08x\n",
        getreg32(config->base + SAM_MCAN_RXF1S_OFFSET),
        getreg32(config->base + SAM_MCAN_RXF1A_OFFSET),
        getreg32(config->base + SAM_MCAN_RXESC_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBC_OFFSET));

  lldbg(" TXFQS: %08x TXESC: %08x TXBRP: %08x  TXBAR: %08x\n",
        getreg32(config->base + SAM_MCAN_TXFQS_OFFSET),
        getreg32(config->base + SAM_MCAN_TXESC_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBRP_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBAR_OFFSET));

  lldbg(" TXBCR: %08x TXBTO: %08x TXBCF: %08x TXBTIE: %08x\n",
        getreg32(config->base + SAM_MCAN_TXBCR_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBTO_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBCF_OFFSET),
        getreg32(config->base + SAM_MCAN_TXBTIE_OFFSET));

  lldbg("TXBCIE: %08x TXEFC: %08x TXEFS: %08x  TXEFA: %08x\n",
        getreg32(config->base + SAM_MCAN_TXBCIE_OFFSET),
        getreg32(config->base + SAM_MCAN_TXEFC_OFFSET),
        getreg32(config->base + SAM_MCAN_TXEFS_OFFSET),
        getreg32(config->base + SAM_MCAN_TXEFA_OFFSET));
}
#endif

/****************************************************************************
 * Name: mcan_dev_lock
 *
 * Description:
 *   Take the semaphore that enforces mutually exclusive access to device
 *   structures, handling any exceptional conditions
 *
 * Input Parameters:
 *   priv - A reference to the MCAN peripheral state
 *
 * Returned Value:
 *  None
 *
 ****************************************************************************/

static void mcan_dev_lock(FAR struct sam_mcan_s *priv)
{
  int ret;

  /* Wait until we successfully get the semaphore.  EINTR is the only
   * expected 'failure' (meaning that the wait for the semaphore was
   * interrupted by a signal.
   */

  do
    {
      ret = sem_wait(&priv->locksem);
      DEBUGASSERT(ret == 0 || errno == EINTR);
    }
  while (ret < 0);
}

/****************************************************************************
 * Name: mcan_buffer_reserve
 *
 * Description:
 *   Take the semaphore that indicates the availability of a TX FIFOQ
 *   buffer, handling any exceptional conditions
 *
 * Input Parameters:
 *   priv - A reference to the MCAN peripheral state
 *
 * Returned Value:
 *  None
 *
 ****************************************************************************/

static void mcan_buffer_reserve(FAR struct sam_mcan_s *priv)
{
  int ret;

  /* Wait until we successfully get the semaphore.  EINTR is the only
   * expected 'failure' (meaning that the wait for the semaphore was
   * interrupted by a signal.
   */

  do
    {
      ret = sem_wait(&priv->txfsem);
      DEBUGASSERT(ret == 0 || errno == EINTR);
    }
  while (ret < 0);
}

/****************************************************************************
 * Name: mcan_recvsetup
 *
 * Description:
 *   Configure and enable mailbox(es) for reception
 *
 * Input Parameter:
 *   priv - A pointer to the private data structure for this MCAN peripheral
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 * Assumptions:
 *   Caller has exclusive access to the MCAN data structures
 *   MCAN interrupts are disabled at the NVIC
 *
 ****************************************************************************/

static int mcan_recvsetup(FAR struct sam_mcan_s *priv)
{
  FAR const struct sam_config_s *config = priv->config;

#warning Missing logic
  return OK;
}

/****************************************************************************
 * Name: mcan_reset
 *
 * Description:
 *   Reset the MCAN device.  Called early to initialize the hardware. This
 *   function is called, before mcan_setup() and on error conditions.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *  None
 *
 ****************************************************************************/

static void mcan_reset(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv;
  FAR const struct sam_config_s *config;

  DEBUGASSERT(dev);
  priv = dev->cd_priv;
  DEBUGASSERT(priv);
  config = priv->config;
  DEBUGASSERT(config);

  canllvdbg("MCAN%d\n", config->port);
  UNUSED(config);

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

  /* Disable all interrupts */

  mcan_putreg(priv, SAM_MCAN_IE_OFFSET, 0);
  mcan_putreg(priv, SAM_MCAN_TXBTIE_OFFSET, 0);

  /* Disable the MCAN controller */
#warning Missing logic
  mcan_dev_unlock(priv);
}

/****************************************************************************
 * Name: mcan_setup
 *
 * Description:
 *   Configure the MCAN. This method is called the first time that the MCAN
 *   device is opened.  This will occur when the port is first opened.
 *   This setup includes configuring and attaching MCAN interrupts.
 *   All MCAN interrupts are disabled upon return.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

static int mcan_setup(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv;
  FAR const struct sam_config_s *config;
  int ret;

  DEBUGASSERT(dev);
  priv = dev->cd_priv;
  DEBUGASSERT(priv);
  config = priv->config;
  DEBUGASSERT(config);

  canllvdbg("MCAN%d pid: %d\n", config->port, config->pid);

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

  /* MCAN hardware initialization */

  ret = mcan_hw_initialize(priv);
  if (ret < 0)
    {
      canlldbg("MCAN%d H/W initialization failed: %d\n", config->port, ret);
      return ret;
    }

  mcan_dumpregs(priv, "After hardware initialization");

  /* Attach the MCAN interrupt handlers */

  ret = irq_attach(config->irq0, config->handler);
  if (ret < 0)
    {
      canlldbg("Failed to attach MCAN%d line 0 IRQ (%d)",
      config->port, config->irq0);
      return ret;
    }

  ret = irq_attach(config->irq1, config->handler);
  if (ret < 0)
    {
      canlldbg("Failed to attach MCAN%d line 1 IRQ (%d)",
      config->port, config->irq1);
      return ret;
    }

  /* Setup receive mailbox(es) (enabling receive interrupts) */

  ret = mcan_recvsetup(priv);
  if (ret < 0)
    {
      canlldbg("MCAN%d H/W initialization failed: %d\n", config->port, ret);
      return ret;
    }

  mcan_dumpregs(priv, "After receive setup");

  /* Enable the interrupts at the NVIC (they are still disabled at the MCAN
   * peripheral). */

  up_enable_irq(config->irq0);
  up_enable_irq(config->irq1);
  mcan_dev_unlock(priv);
  return OK;
}

/****************************************************************************
 * Name: mcan_shutdown
 *
 * Description:
 *   Disable the MCAN.  This method is called when the MCAN device is closed.
 *   This method reverses the operation the setup method.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void mcan_shutdown(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv;
  FAR const struct sam_config_s *config;

  DEBUGASSERT(dev);
  priv = dev->cd_priv;
  DEBUGASSERT(priv);
  config = priv->config;
  DEBUGASSERT(config);

  canllvdbg("MCAN%d\n", config->port);

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

  /* Disable the MCAN interrupts */

  up_disable_irq(config->irq0);
  up_disable_irq(config->irq1);

  /* Detach the MCAN interrupt handler */

  irq_detach(config->irq0);
  irq_detach(config->irq1);

  /* And reset the hardware */

  mcan_reset(dev);
  mcan_dev_unlock(priv);
}

/****************************************************************************
 * Name: mcan_rxint
 *
 * Description:
 *   Call to enable or disable RX interrupts.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void mcan_rxint(FAR struct can_dev_s *dev, bool enable)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  irqstate_t flags;
  uint32_t regval;

  DEBUGASSERT(priv && priv->config);

  canllvdbg("MCAN%d enable: %d\n", priv->config->port, enable);

  /* Enable/disable the receive interrupts */

  flags = irqsave();
  regval = mcan_getreg(priv, SAM_MCAN_IE_OFFSET);

  if (enable)
    {
      regval |= priv->rxints | MCAN_COMMON_INTS;
    }
  else
    {
      regval &= ~priv->rxints;
    }

  mcan_putreg(priv, SAM_MCAN_IE_OFFSET, regval);
  irqrestore(flags);
}

/****************************************************************************
 * Name: mcan_txint
 *
 * Description:
 *   Call to enable or disable TX interrupts.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void mcan_txint(FAR struct can_dev_s *dev, bool enable)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  irqstate_t flags;
  uint32_t regval;

  DEBUGASSERT(priv && priv->config);

  canllvdbg("MCAN%d enable: %d\n", priv->config->port, enable);

  /* Enable/disable the receive interrupts */

  flags = irqsave();
  regval = mcan_getreg(priv, SAM_MCAN_IE_OFFSET);

  if (enable)
    {
      regval |= priv->txints | MCAN_COMMON_INTS;
      priv->txenabled = true;
    }
  else
    {
      regval &= ~priv->txints;
      priv->txenabled = false;
    }

  mcan_putreg(priv, SAM_MCAN_IE_OFFSET, regval);
  irqrestore(flags);
}

/****************************************************************************
 * Name: mcan_ioctl
 *
 * Description:
 *   All ioctl calls will be routed through this method
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

static int mcan_ioctl(FAR struct can_dev_s *dev, int cmd, unsigned long arg)
{
  /* No CAN ioctls are supported */

  return -ENOTTY;
}

/****************************************************************************
 * Name: mcan_remoterequest
 *
 * Description:
 *   Send a remote request
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

static int mcan_remoterequest(FAR struct can_dev_s *dev, uint16_t id)
{
  /* REVISIT:  Remote request not implemented */

  return -ENOSYS;
}

/****************************************************************************
 * Name: mcan_send
 *
 * Description:
 *    Send one can message.
 *
 *    One CAN-message consists of a maximum of 10 bytes.  A message is
 *    composed of at least the first 2 bytes (when there are no data bytes).
 *
 *    Byte 0:      Bits 0-7: Bits 3-10 of the 11-bit CAN identifier
 *    Byte 1:      Bits 5-7: Bits 0-2 of the 11-bit CAN identifier
 *                 Bit 4:    Remote Transmission Request (RTR)
 *                 Bits 0-3: Data Length Code (DLC)
 *    Bytes 2-10: CAN data
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

static int mcan_send(FAR struct can_dev_s *dev, FAR struct can_msg_s *msg)
{
  FAR struct sam_mcan_s *priv;
  FAR const struct sam_config_s *config = priv->config;
  FAR uint32_t *txbuffer = 0;
  FAR const uint8_t *src;
  FAR uint8_t *dest;
  irqstate_t flags;
  uint32_t regval;
  unsigned int msglen;
  unsigned int ndx;
  unsigned int i;

  DEBUGASSERT(dev);
  priv = dev->cd_priv;
  DEBUGASSERT(priv && priv->config);
  config = priv->config;

  canllvdbg("MCAN%d\n", config->port);
  canllvdbg("MCAN%d ID: %d DLC: %d\n",
            config->port, msg->cm_hdr.ch_id, msg->cm_hdr.ch_dlc);

  /* That that FIFO elements were configured.
   *
   * REVISIT: Dedicated TX buffers are not used by this driver.
   */

  DEBUGASSERT(config->ntxfifoq > 0);

  /* Reserve a buffer for the transmission, waiting if necessary.  When
   * mcan_buffer_reserve() returns, we are guaranteed the the TX FIFOQ is
   * not full and cannot become full at least until we add our packet to
   * the FIFO.
   *
   * REVISIT: This needs to be extended in order to handler case where
   * the MCAN device was opened O_NONBLOCK.
   */

  mcan_buffer_reserve(priv);

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

  /* Get our reserved Tx FIFO/queue put index */

  regval = mcan_getreg(priv, SAM_MCAN_TXFQS_OFFSET);
  DEBUGASSERT((regval & MCAN_TXFQS_TFQF) == 0);

  ndx = (regval & MCAN_TXFQS_TFQPI_MASK) >> MCAN_TXFQS_TFQPI_SHIFT;

  /* And the TX buffer corresponding to this index */

  txbuffer = config->msgram.txdedicated + ndx * config->txbufferesize;

  /* Format the TX FIFOQ entry
   *
   * Format word T1:
   *   Transfer message ID (ID)          - Value from message structure
   *   Remote Transmission Request (RTR) - Value from message structure
   *   Extended Identifier (XTD)         - Depends on configuration.
   */

#ifdef CONFIG_CAN_EXTID
  DEBUGASSERT(msg->cm_hdr.ch_extid);
  DEBUGASSERT(msg->cm_hdr.ch_id < (1 << 29));

  regval = BUFFER_R0_EXTID(msg->cm_hdr.ch_id) | BUFFER_R0_XTD;
#else
  DEBUGASSERT(!msg->cm_hdr.ch_extid);
  DEBUGASSERT(msg->cm_hdr.ch_id < (1 << 11));

  regval = BUFFER_R0_STDID(msg->cm_hdr.ch_id);
#endif

  if (msg->cm_hdr.ch_rtr)
    {
      regval |= BUFFER_R0_RTR;
    }

  txbuffer[0] = regval;

  /* Format word T1:
   *   Data Length Code (DLC)            - Value from message structure
   *   Event FIFO Control (EFC)          - Do not store events.
   *   Message Marker (MM)               - Always zero
   */

  txbuffer[1] = BUFFER_R1_DLC(msg->cm_hdr.ch_dlc);

  /* Followed by the amount of data corresponding to the DLC (T2..) */

  dest = (FAR uint8_t*)&txbuffer[2];
  src  = msg->cm_data;

  for (i = 0; i < msg->cm_hdr.ch_dlc; i++)
    {
      /* Little endian is assumed */

      *dest++ = *src++;
    }

  /* Flush the D-Cache to memory before initiating the tranfer */

  msglen = 2 * sizeof(uint32_t) + msg->cm_hdr.ch_dlc;
  arch_clean_dcache((uintptr_t)txbuffer, (uintptr_t)txbuffer + msglen);
  UNUSED(msglen);

  /* Enable transmit interrupts from the TX FIFOQ buffer by setting TC
   * interrupt bit in IR (also requires that the TC interrupt is enabled)
   */

  mcan_putreg(priv, SAM_MCAN_TXBTIE_OFFSET, (1 << ndx));

  /* And request to send the packet */

  mcan_putreg(priv, SAM_MCAN_TXBAR_OFFSET, (1 << ndx));

  /* If we have not been asked to suppress TX interrupts, then enable
   * TX interrupts now.
   */

  if (priv->txenabled)
    {
      flags   = irqsave();
      regval  = mcan_getreg(priv, SAM_MCAN_IE_OFFSET);
      regval |= priv->txints;
      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, priv->txints);
      irqrestore(flags);
    }

  mcan_dev_unlock(priv);
  return OK;
}

/****************************************************************************
 * Name: mcan_txready
 *
 * Description:
 *   Return true if the MCAN hardware can accept another TX message.
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   True if the MCAN hardware is ready to accept another TX message.
 *
 ****************************************************************************/

static bool mcan_txready(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  bool txready;

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

#warning Missing logic

  mcan_dev_unlock(priv);
  return txready;
}

/****************************************************************************
 * Name: mcan_txempty
 *
 * Description:
 *   Return true if all message have been sent.  If for example, the MCAN
 *   hardware implements FIFOs, then this would mean the transmit FIFO is
 *   empty.  This method is called when the driver needs to make sure that
 *   all characters are "drained" from the TX hardware before calling
 *   co_shutdown().
 *
 * Input Parameters:
 *   dev - An instance of the "upper half" can driver state structure.
 *
 * Returned Value:
 *   True if there are no pending TX transfers in the MCAN hardware.
 *
 ****************************************************************************/

static bool mcan_txempty(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  bool txempty;

  /* Get exclusive access to the MCAN peripheral */

  mcan_dev_lock(priv);

#warning Missing logic

  mcan_dev_unlock(priv);
  return txempty;
}

/****************************************************************************
 * Name: mcan_dedicated_rxbuffer_available
 *
 * Description:
 *   Check if data is available in a dedicated RX buffer.
 *
 * Input Parameters:
 *   priv   - MCAN-specific private data
 *   bufndx - Buffer index
 *
 *   None
 * Returned Value:
 *   True: Data is available
 *
 ***************************************************************************/

#if 0 /* Not Used */
bool mcan_dedicated_rxbuffer_available(FAR struct sam_mcan_s *priv, int bufndx)
{
  if (bufndx < 32)
    {
      return (bool)(mcan->MCAN_NDAT1 & (1 << bufndx));
    }
  else if (bufndx < 64)
    {
      return (bool)(mcan->MCAN_NDAT1 & (1 << (bufndx - 32)));
    }
  else
    {
      return false;
    }
}
#endif

/****************************************************************************
 * Name: mcan_receive
 *
 * Description:
 *   Receive an MCAN messages
 *
 * Input Parameters:
 *   dev - CAN-common state data
 *   rxbuffer - The RX buffer containing the received messages
 *   nbytes   - The length of the received message
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void mcan_receive(FAR struct can_dev_s *dev, FAR uint32_t *rxbuffer,
                         unsigned long nbytes)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  struct can_hdr_s hdr;
  uint32_t regval;
  int ret;

  /* Invalidate the D-Cache so that we reread the RX buffer data from memory. */

  arch_invalidate_dcache((uintptr_t)rxbuffer, (uintptr_t)rxbuffer + nbytes);

  /* Format the CAN header */
  /* Work R0 contains the CAN ID */

  regval        = *rxbuffer++;

  hdr.ch_rtr    = 0;
#ifdef CONFIG_CAN_EXTID
  hdr.ch_unused = 0;

  if ((regval & BUFFER_R0_XTD) != 0)
    {
      /* Save the extended ID of the newly received message */

      hdr.ch_id     = (regval & BUFFER_R0_EXTID_MASK) >> BUFFER_R0_EXTID_SHIFT;
      hdr.ch_extid  = true;
    }
  else
    {
      hdr.ch_id     = (regval & BUFFER_R0_STDID_MASK) >> BUFFER_R0_STDID_SHIFT;
      hdr.ch_extid  = false;
    }
#else
  if ((regval & BUFFER_R0_XTD) != 0)
    {
      /* Drop any messages with extended IDs */

      return;
    }

  /* Save the standard ID of the newly received message */

  hdr.ch_id  = (regval & BUFFER_R0_STDID_MASK) >> BUFFER_R0_STDID_SHIFT;
#endif

  /* Word R1 contains the DLC and timestamp */

  regval     = *rxbuffer++;
  hdr.ch_dlc = (regval & BUFFER_R1_DLC_MASK) >> BUFFER_R1_DLC_SHIFT;

  /* And provide the CAN message to the upper half logic */

  ret = can_receive(dev, &hdr, (FAR uint8_t *)rxbuffer);
  if (ret < 0)
    {
      canlldbg("ERROR: can_receive failed: %d\n", ret);
    }
}

/****************************************************************************
 * Name: mcan_interrupt
 *
 * Description:
 *   Common MCAN interrupt handler
 *
 * Input Parameters:
 *   dev - CAN-common state data
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void mcan_interrupt(FAR struct can_dev_s *dev)
{
  FAR struct sam_mcan_s *priv = dev->cd_priv;
  FAR const struct sam_config_s *config;
  uint32_t ir;
  uint32_t ie;
  uint32_t pending;
  uint32_t regval;
  unsigned int nelem;
  unsigned int ndx;

  DEBUGASSERT(priv && priv->config);
  config = priv->config;

  /* Get the set of pending interrupts. */

  ir = mcan_getreg(priv, SAM_MCAN_IR_OFFSET);
  ie = mcan_getreg(priv, SAM_MCAN_IE_OFFSET);

  pending = (ir & ie);

  /* Check for common errors */

  if ((pending & MCAN_CMNERR_INTS) != 0)
    {
      canlldbg("ERROR: Common %08x\n", pending & MCAN_CMNERR_INTS);

      /* Clear the error indications */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_CMNERR_INTS);
    }

  /* Check for transmission errors */

  if ((pending & MCAN_TXERR_INTS) != 0)
    {
      canlldbg("ERROR: TX %08x\n", pending & MCAN_TXERR_INTS);

      /* Clear the error indications */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_TXERR_INTS);
    }

  /* Check for successful completion of a transmission */

  if ((pending & MCAN_INT_TC) != 0)
    {
      /* Clear the pending TX completion interrupt (and all
       * other TX-related interrupts)
       */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, priv->txints);

      /* Indicate that there is one more buffer free in the TX FIFOQ by
       * "releasing" it.  This may have the effect of waking up a thread
       * that has been waiting for a free TX FIFOQ buffer.
       *
       * REVISIT: TX dedicated buffers are not supported.
       */

      mcan_buffer_release(priv);
      DEBUGASSERT(priv->txfsem.semcount <= priv->ntxfifoq);
    }
  else if ((pending & priv->txints) != 0)
    {
      /* Clear unhandled TX events */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, priv->txints);
    }

#if 0 /* Not used */
  /* Check if a message has been stored to the dedicated RX buffer (DRX) */

  if ((pending & MCAN_INT_DRX) != 0))
    {
      int i;

      /* Clear the pending DRX interrupt */

      mcan_putreg(priv, SAM_MCAN_IR_OFFSET, MCAN_INT_DRX);

      /* Process each dedicated RX buffer */

      for (i = 0; i < config->nrxdedicated; i++)
        {
          uint32_t *rxdedicated = &config->rxdedicated[i];

          /* Check if datat is available in this dedicated RX buffer */

          if (mcan_dedicated_rxbuffer_available(priv, i))
            {
              /* Yes.. Invalidate the D-Cache to that data will be re-
               * fetched from RAM.
               *
               * REVISIT:  This will require 32-byte alignment.
               */

              arch_invalidata_dcache();
              mcan_dedicated_rxbuffer_receive(priv);
            }
        }
    }
#endif

  /* Check for reception errors */

  if ((pending & MCAN_RXERR_INTS) != 0)
    {
      canlldbg("ERROR: RX %08x\n", pending & MCAN_RXERR_INTS);

      /* Clear the error indications */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_RXERR_INTS);
    }

  /* Check for successful reception of a new message in RX FIFO0 */

  if ((pending & MCAN_INT_RF0N) != 0)
    {
      DEBUGASSERT(priv->txfsem.semcount <= priv->nrxfifo0);

      /* Clear the RX FIFO0 interrupt (and all other FIFO0-related
       * interrupts)
       */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_RXFIFO0_INTS);
      pending &= ~MCAN_RXFIFO0_INTS;

      /* Handle the newly received message in FIFO0 */

      regval = mcan_getreg(priv, SAM_MCAN_RXF0S_OFFSET);

      if ((regval & MCAN_RXF0S_RF0L) != 0)
        {
          canlldbg("ERROR: Message lost: %08x\n", regval);
        }
      else
        {
          ndx    = (regval & MCAN_RXF0S_F0GI_MASK) >> MCAN_RXF0S_F0GI_SHIFT;
          nelem  = (regval & MCAN_RXF0S_F0FL_MASK) >> MCAN_RXF0S_F0FL_SHIFT;

          if (nelem > 0)
            {
              mcan_receive(dev,
                           config->msgram.rxfifo0 +
                             (ndx * priv->config->rxfifo0esize),
                           nelem * priv->config->rxfifo0esize);
            }
        }

      /* Acknowledge reading the FIFO entry */

      mcan_putreg(priv, SAM_MCAN_RXF0A_OFFSET, ndx);
    }

  /* Check for successful reception of a new message in RX FIFO1 */

  if ((pending & MCAN_INT_RF1N) != 0)
    {
      DEBUGASSERT(priv->txfsem.semcount <= priv->nrxfifo1);

      /* Clear the RX FIFO1 interrupt (and all other FIFO1-related
       * interrupts)
       */

      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_RXFIFO1_INTS);
      pending &= ~MCAN_RXFIFO1_INTS;

      /* Handle the newly received message in FIFO1 */

      regval = mcan_getreg(priv, SAM_MCAN_RXF1S_OFFSET);

      if ((regval & MCAN_RXF0S_RF0L) != 0)
        {
          canlldbg("ERROR: Message lost: %08x\n", regval);
        }
      else
        {
          ndx    = (regval & MCAN_RXF1S_F1GI_MASK) >> MCAN_RXF1S_F1GI_SHIFT;
          nelem  = (regval & MCAN_RXF1S_F1FL_MASK) >> MCAN_RXF1S_F1FL_SHIFT;

          if (nelem > 0)
            {
              mcan_receive(dev,
                           config->msgram.rxfifo1 +
                             (ndx * priv->config->rxfifo1esize),
                           nelem * priv->config->rxfifo1esize);
            }
        }

      /* Acknowledge reading the FIFO entry */

      mcan_putreg(priv, SAM_MCAN_RXF1A_OFFSET, ndx);
    }

  /* Clear unhandled RX interrupts */

  if ((pending & priv->rxints) != 0)
    {
      mcan_putreg(priv, SAM_MCAN_IE_OFFSET, priv->rxints);
    }
}

/****************************************************************************
 * Name: mcan0_interrupt
 *
 * Description:
 *   MCAN0 interrupt handler
 *
 * Input Parameters:
 *   irq     - The IRQ number of the interrupt.
 *   context - The register state save array at the time of the interrupt.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

#ifdef CONFIG_SAMV7_MCAN0
static int mcan0_interrupt(int irq, void *context)
{
  mcan_interrupt(&g_mcan0dev);
  return OK;
}
#endif

/****************************************************************************
 * Name: mcan1_interrupt
 *
 * Description:
 *   MCAN1 interrupt handler
 *
 * Input Parameters:
 *   irq     - The IRQ number of the interrupt.
 *   context - The register state save array at the time of the interrupt.
 *
 * Returned Value:
 *   Zero on success; a negated errno on failure
 *
 ****************************************************************************/

#ifdef CONFIG_SAMV7_MCAN1
static int mcan1_interrupt(int irq, void *context)
{
  mcan_interrupt(&g_mcan1dev);
  return OK;
}
#endif

/****************************************************************************
 * Name: mcan_configure_interrupts
 *
 * Description:
 *   Configure interrupt lines.
 *
 * Input Parameter:
 *   priv - A pointer to the private data structure for this MCAN peripheral
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static void mcan_configure_interrupts(FAR struct sam_mcan_s *priv)
{
  uint32_t regval;

  /* Select RX-related interrupts */

#if 0 /* Dedicated RX buffers are not used by this driver */
  priv->rxints = MCAN_RXDEDBUF_INTS | MCAN_COMMON_INTS;
#else
  priv->rxints = MCAN_RXFIFO_INTS | MCAN_COMMON_INTS;
#endif

  /* Select RX-related interrupts */

#if 0 /* Dedicated RX buffers are not used by this driver */
#else
#  warning Missing Logic
#endif

  /* Direct to Line 0.
   * REVISIT: Only interrupt line 0 is used by this driver.
   */

  regval  = mcan_getreg(priv, SAM_MCAN_ILS_OFFSET);
  regval |= (priv->rxints | priv->txints | MCAN_COMMON_INTS);
  mcan_putreg(priv, SAM_MCAN_ILS_OFFSET, regval);

  /* Make sure that interrupt line 0 is enabled. */

  regval  = mcan_getreg(priv, SAM_MCAN_ILE_OFFSET);
  regval |= MCAN_ILE_EINT0;
  mcan_putreg(priv, SAM_MCAN_ILE_OFFSET, regval);

  /* Clear any pending interrupts but do not yet enable a interrupt */

  mcan_putreg(priv, SAM_MCAN_IE_OFFSET, MCAN_INT_ALL);
}

/****************************************************************************
 * Name: mcan_hw_initialize
 *
 * Description:
 *   MCAN hardware initialization
 *
 * Input Parameter:
 *   priv - A pointer to the private data structure for this MCAN peripheral
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int mcan_hw_initialize(struct sam_mcan_s *priv)
{
  FAR const struct sam_config_s *config = priv->config;
  FAR uint32_t *msgram;
  uint32_t regval;
  uint32_t cntr;
  uint32_t cmr;

  canllvdbg("MCAN%d\n", config->port);

  /* Configure MCAN pins */

  sam_configgpio(config->rxpinset);
  sam_configgpio(config->txpinset);

  /* Enable peripheral clocking */

  sam_enableperiph1(config->pid);

  /* Enable the Initialization state */

  regval  = mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET);
  regval |= MCAN_CCCR_INIT;
  mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);

  /* Wait for initialization mode to take effect */

  while ((mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET) & MCAN_CCCR_INIT) == 0);

  /* Enable writing to configuration registers */

  regval  = mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET);
  regval |= (MCAN_CCCR_INIT | MCAN_CCCR_CCE);
  mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);

  /* Global Filter Configuration: Reject remote frames, reject non-matching
   * frames.
   */

  regval = MCAN_GFC_RRFE | MCAN_GFC_RRFS | MCAN_GFC_ANFE_REJECTED |
           MCAN_GFC_ANFS_REJECTED;
  mcan_putreg(priv, SAM_MCAN_GFC_OFFSET, regval);

  /* Extended ID Filter AND mask  */

  mcan_putreg(priv, SAM_MCAN_XIDAM_OFFSET, 0x1fffffff);

  /* Disable all interrupts  */

  mcan_putreg(priv, SAM_MCAN_IE_OFFSET, 0);
  mcan_putreg(priv, SAM_MCAN_TXBTIE_OFFSET, 0);

  /* All interrupts directed to Line 0.  But disable both interrupt lines 0
   * and 1 for now.
   *
   * REVISIT: Only interrupt line 0 is used by this driver.
   */

  mcan_putreg(priv, SAM_MCAN_ILS_OFFSET, 0);
  mcan_putreg(priv, SAM_MCAN_ILE_OFFSET, 0);

  /* Clear all pending interrupts. */

  mcan_putreg(priv, SAM_MCAN_IR_OFFSET, MCAN_INT_ALL);

  /* Configure MCAN bit timing */

  mcan_putreg(priv, SAM_MCAN_BTP_OFFSET, config->btp);
  mcan_putreg(priv, SAM_MCAN_FBTP_OFFSET, config->fbtp);

  /* Configure message RAM starting addresses and sizes. */

  regval = MAILBOX_ADDRESS(config->msgram.stdfilters) |
           MCAN_SIDFC_LSS(config->nstdfilters);
  mcan_putreg(priv, SAM_MCAN_SIDFC_OFFSET, regval);

  regval = MAILBOX_ADDRESS(config->msgram.extfilters) |
           MCAN_XIDFC_LSE(config->nextfilters);
  mcan_putreg(priv, SAM_MCAN_XIDFC_OFFSET, regval);

  /* Configure RX FIFOs */

  regval = MAILBOX_ADDRESS(config->msgram.rxfifo0) |
           MCAN_RXF0C_F0S(config->nfifo0);
  mcan_putreg(priv, SAM_MCAN_RXF0C_OFFSET, regval);

  regval = MAILBOX_ADDRESS(config->msgram.rxfifo1) |
           MCAN_RXF1C_F1S(config->nfifo1);
  mcan_putreg(priv, SAM_MCAN_RXF1C_OFFSET, regval);

  /* Watermark interrupt off, blocking mode */

  regval = MAILBOX_ADDRESS(config->msgram.rxdedicated);
  mcan_putreg(priv, SAM_MCAN_RXBC_OFFSET, regval);

  regval = MAILBOX_ADDRESS(config->msgram.txeventfifo) |
           MCAN_TXEFC_EFS(config->ntxeventfifo);
  mcan_putreg(priv, SAM_MCAN_TXEFC_OFFSET, regval);

  /* Watermark interrupt off */

  regval = MAILBOX_ADDRESS(config->msgram.txdedicated) |
           MCAN_TXBC_NDTB(config->ntxdedicated) |
           MCAN_TXBC_TFQS(config->ntxfifoq);
  mcan_putreg(priv, SAM_MCAN_TXBC_OFFSET, regval);

  regval = MCAN_RXESC_RBDS(config->rxbufferecode) |
           MCAN_RXESC_F1DS(config->rxfifo1ecode) |
           MCAN_RXESC_F0DS(config->rxfifo0ecode);
  mcan_putreg(priv, SAM_MCAN_RXESC_OFFSET, regval);

  regval = MCAN_TXESC_TBDS(config->txbufferesize);
  mcan_putreg(priv, SAM_MCAN_TXESC_OFFSET, regval);

  /* Configure Message Filters */
  /* Disable all standard filters */

  msgram = config->msgram.stdfilters;
  cntr   = config->nstdfilters;
  while (cntr > 0)
    {
      *msgram++ = STDFILTER_S0_SFEC_DISABLE;
      cntr--;
    }

  /* Disable all extended filters */

  msgram = config->msgram.extfilters;
  cntr = config->nextfilters;
  while (cntr > 0)
    {
      *msgram = EXTFILTER_F0_EFEC_DISABLE;
      msgram = msgram + 2;
      cntr--;
    }

  /* Clear new RX data flags */

  mcan_putreg(priv, SAM_MCAN_NDAT1_OFFSET, 0xffffffff);
  mcan_putreg(priv, SAM_MCAN_NDAT2_OFFSET, 0xffffffff);

  /* Select ISO11898-1 mode or FD mode with or without fast bit rate
   * switching
   */

  regval  = mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET);
  regval &= ~(MCAN_CCCR_CME_MASK | MCAN_CCCR_CMR_MASK);

  switch (config->mode)
    {
    default:
    case MCAN_ISO11898_1_MODE:
      regval |= MCAN_CCCR_CME_ISO11898_1;
      cmr     = MCAN_CCCR_CMR_ISO11898_1;
      break;

    case MCAN_FD_MODE:
      regval |= MCAN_CCCR_CME_FD;
      cmr     = MCAN_CCCR_CMR_FD;
      break;

    case MCAN_FD_BSW_MODE:
      regval |= MCAN_CCCR_CME_FD_BSW;
      cmr     = MCAN_CCCR_CMR_FD_BSW;
      break;
    }

  /* Set the initial CAN mode */

  mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);

  /* Request the mode change */

  regval |= cmr;
  mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);

#if 0 /* Not necessary in initialization mode */
  /* Wait for the mode to take effect */

  while ((mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET) & (MCAN_CCCR_FDBS | MCAN_CCCR_FDO)) != 0);
#endif

  /* Enable FIFO/Queue mode
   *
   * REVISIT: Dedicated TX buffers are not used.
   */

  regval  = mcan_getreg(priv, SAM_MCAN_TXBC_OFFSET);
  regval |= MCAN_TXBC_TFQM;
  mcan_putreg(priv, SAM_MCAN_TXBC_OFFSET, regval);

#ifdef SAMV7_MCAN_LOOPBACK
  /* Is loopback mode selected for this peripheral? */

  if (config->loopback)
    {
     /* MCAN_CCCR_TEST  - Test mode enable
      * MCAN_CCCR_MON   - Bus monitoring mode (for internal loopback)
      * MCAN_TEST_LBCK  - Loopback mode
      */

      regval = mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET);
      regval |= (MCAN_CCCR_TEST | MCAN_CCCR_MON);
      mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);

      regval = mcan_getreg(priv, SAM_MCAN_TEST_OFFSET);
      regval |= MCAN_TEST_LBCK;
      mcan_putreg(priv, SAM_MCAN_TEST_OFFSET, regval);
    }
#endif

  /* Configure interrupt lines */

  mcan_configure_interrupts(priv);

  /* Disable initialization mode to enable normal operation */

  regval  = mcan_getreg(priv, SAM_MCAN_CCCR_OFFSET);
  regval &= ~MCAN_CCCR_INIT;
  mcan_putreg(priv, SAM_MCAN_CCCR_OFFSET, regval);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sam_mcan_initialize
 *
 * Description:
 *   Initialize the selected MCAN port
 *
 * Input Parameter:
 *   port - Port number (for hardware that has multiple MCAN interfaces),
 *          0=MCAN0, 1=MCAN1
 *
 * Returned Value:
 *   Valid CAN device structure reference on success; a NULL on failure
 *
 ****************************************************************************/

FAR struct can_dev_s *sam_mcan_initialize(int port)
{
  FAR struct can_dev_s *dev;
  FAR struct sam_mcan_s *priv;
  FAR const struct sam_config_s *config;
  uint32_t regval;

  canvdbg("MCAN%d\n", port);

  /* Select PCK5 clock source and pre-scaler value.  Both MCAN controllers
   * use PCK5 to derive bit rate.
   */

  regval = PMC_PCK_PRES(CONFIG_SAMV7_MCAN_CLKSRC_PRESCALER) | SAMV7_MCAN_CLKSRC;
  putreg32(regval, SAM_PMC_PCK5);

  /* Enable PCK5 */

  putreg32(PMC_PCK5, SAM_PMC_SCER);

  /* Select MCAN peripheral to be initialized */

#ifdef CONFIG_SAMV7_MCAN0
  if (port == MCAN0)
    {
      /* Select the MCAN0 device structure */

      dev    = &g_mcan0dev;
      priv   = &g_mcan0priv;
      config = &g_mcan0const;

      /* Configure MCAN0 Message RAM Base Address */

      regval  = getreg32(SAM_MATRIX_CAN0);
      regval &= MATRIX_CAN0_RESERVED;
      regval |= (uint32_t)config->msgram.stdfilters & MATRIX_CAN0_CAN0DMABA_MASK;
      putreg32(regval, SAM_MATRIX_CAN0);
    }
  else
#endif
#ifdef CONFIG_SAMV7_MCAN1
  if (port == MCAN1)
    {
      /* Select the MCAN1 device structure */

      dev    = &g_mcan1dev;
      priv   = &g_mcan1priv;
      config = &g_mcan1const;

      /* Configure MCAN1 Message RAM Base Address */

      regval  = getreg32(SAM_MATRIX_CCFG_SYSIO);
      regval &= ~MATRIX_CCFG_CAN1DMABA_MASK;
      regval |= (uint32_t)config->msgram.stdfilters & MATRIX_CCFG_CAN1DMABA_MASK;
      putreg32(regval, SAM_MATRIX_CCFG_SYSIO);
    }
  else
#endif
    {
      candbg("ERROR: Unsupported port %d\n", port);
      return NULL;
    }

  /* Is this the first time that we have handed out this device? */

  if (!priv->initialized)
    {
      /* Yes, then perform one time data initialization */

      memset(priv, 0, sizeof(struct sam_mcan_s));
      priv->config      = config;
      priv->initialized = true;

      sem_init(&priv->locksem, 0, 1);
      sem_init(&priv->txfsem, 0, config->ntxfifoq);

      dev->cd_ops       = &g_mcanops;
      dev->cd_priv      = (FAR void *)priv;

      /* And put the hardware in the initial state */

      mcan_reset(dev);
    }

  return dev;
}

#endif /* CONFIG_CAN && CONFIG_SAMV7_MCAN */
