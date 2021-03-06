/****************************************************************************
 * boards/z80/ez80/z20x/src/w25_main.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <debug.h>
#include <hex2bin.h>

#include <nuttx/streams.h>
#include <arch/irq.h>

#include "z20x.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Configuration ************************************************************/

#ifndef HAVE_SPIFLASH
#  error The W25 Serial FLASH is not available
#endif

#ifndef CONFIG_Z20X_W25_CHARDEV
#  error CONFIG_Z20X_W25_CHARDEV must be selected
#endif

#if !defined(CONFIG_Z20X_W25_PROGSIZE) || CONFIG_Z20X_W25_PROGSIZE < 128
#  error Large CONFIG_Z20X_W25_PROGSIZE must be selected
#endif

#define IOBUFFER_SIZE   512
#define PROG_MAGIC      0xfedbad

#define SRAM_ENTRY      ((sram_entry_t)PROGSTART)

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef CODE void (*sram_entry_t)(void);

/* This is the header places at the beginning of the binary data in FLASH */

struct prog_header_s
{
  uint24_t magic;  /* Valid if PROG_MAGIC */
  uint24_t crc;    /* Last 24-bits of CRC32 */
  uint24_t len;    /* Binary length */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: w25_read_hex
 *
 * Description:
 *   Read and parse a HEX data stream into RAM.  HEX data will be read from
 *   stdin and written to the program area reserved in RAM, that is, the
 *   address range from PROGSTART to PROGREND.
 *
 ****************************************************************************/

static int w25_read_hex(FAR uint24_t *len)
{
  struct lib_rawinstream_s rawinstream;
  struct lib_memsostream_s memoutstream;
  int fd;
  int ret;

  /* Open the W25 device for writing */

  fd = open(W25_CHARDEV, O_WRONLY);
  if (fd < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed to open %s: %d\n", W25_CHARDEV, ret);
      return ret;
    }

  /* Wrap stdin as an IN stream that can get the HEX data over the serial port */

  lib_rawinstream(&rawinstream, 0);

  /* Wrap the memory area used to hold the program as a seek-able OUT stream in
   * which we can buffer the binary data.
   */

  lib_memsostream(&memoutstream, (FAR char *)PROGSTART, PROGSIZE);

  /* We are ready to load the Intel HEX stream into external SRAM.
   *
   * Hmm.. With no hardware handshake, there is a possibility of data loss
   * to overrunning incoming data buffer.  So far I have not seen this at
   * 115200 8N1, but still it is a possibility.
   */

  printf("Send Intel HEX file now\n");
  fflush(stdout);

  ret = hex2bin(&rawinstream.public, &memoutstream.public,
                (uint32_t)PROGSTART, (uint32_t)(PROGSTART + PROGSIZE),
                0);

  close(fd);
  if (ret < 0)
    {
      /* We failed the load */

      fprintf(stderr, "ERROR: Intel HEX file load failed: %d\n", ret);
      return ret;
    }

  printf("Successfully loaded the Intel HEX file into memory...\n");
  *len = memoutstream.public.nput;
  return OK;
}

/****************************************************************************
 * Name: w25_write
 *
 * Description:
 *   Write to the open file descriptor, handling failures and repeated
 *   write operations as necessary.
 *
 ****************************************************************************/

static int w25_write(int fd, FAR const void *src, size_t len)
{
  ssize_t remaining = len;
  ssize_t nwritten;

  do
    {
      nwritten = write(fd, src, remaining);
      if (nwritten <= 0)
        {
          int errcode = get_errno();
          if (errno != EINTR)
            {
              fprintf(stderr, "ERROR: Write failed: %d\n", errcode);
              return -errcode;
            }
        }
      else
        {
          remaining -= nwritten;
          src += nwritten;
        }
    }
  while (remaining > 0);

  return OK;
}

/****************************************************************************
 * Name: w25_write_binary
 *
 * Description:
 *   Write a program header followed by the binary data beginning at address
 *   PROGSTART into the first partition of the w25 FLASH memory.
 *
 ****************************************************************************/

static int w25_write_binary(FAR const struct prog_header_s *hdr)
{
  int fd;
  int ret;

  /* Open the W25 device for writing */

  fd = open(W25_CHARDEV, O_WRONLY);
  if (fd < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed to open %s: %d\n", W25_CHARDEV, ret);
      return ret;
    }

  /* Write the hdr to the W25 */

  ret = w25_write(fd, hdr, sizeof(struct prog_header_s));
  if (fd < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed write program header: %d\n", ret);
      goto errout;
    }

  printf("Writing %lu bytes to the W25 Serial FLASH\n", (unsigned long)hdr->len);

  ret = w25_write(fd, (FAR const void *)PROGSTART, hdr->len);
  if (ret < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed write program header: %d\n", ret);
    }

errout:
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: w25_read
 *
 * Description:
 *   Read from the open file descriptor, handling errors as retries as
 *   necessary.
 *
 ****************************************************************************/

static int w25_read(int fd, FAR void *dest, size_t len)
{
  ssize_t remaining = len;
  ssize_t nread;

  do
    {
      nread = read(fd, dest, remaining);
      if (nread <= 0)
        {
          int errcode = get_errno();
          if (errno != EINTR)
            {
              fprintf(stderr, "ERROR: Read failed: %d\n", errcode);
              close(fd);
              return -errcode;
            }
        }

      remaining -= nread;
      dest += nread;
    }
  while (remaining > 0);

  return OK;
}

/****************************************************************************
 * Name: w25_read_binary
 *
 * Description:
 *   Read the program in the first partition of the W25 part into memory
 *   at PROGSTART.
 *
 ****************************************************************************/

static int w25_read_binary(FAR struct prog_header_s *hdr)
{
  int fd;
  int ret;

  /* Open the W25 device for reading */

  fd = open(W25_CHARDEV, O_RDONLY);
  if (fd < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed to open %s: %d\n", W25_CHARDEV, ret);
      return ret;
    }

  /* Read the header at the beginning of the partition */

  ret = w25_read(fd, hdr, sizeof(hdr));
  if (ret < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed read program header: %d\n", ret);
      goto errout;
    }

  /* Check for a valid program header */

  if (hdr->magic != PROG_MAGIC)
    {
      fprintf(stderr, "ERROR: No program in FLASH\n");
      ret = -ENOENT;
      goto errout;
    }

  if (hdr->len != PROGSIZE)
    {
      fprintf(stderr, "ERROR: Program too big\n");
      ret = -E2BIG;
      goto errout;
    }

  /* Read the program binary */

  ret = w25_read(fd, (FAR void *)PROGSTART, hdr->len);
  if (ret < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed read program header: %d\n", ret);
    }

errout:
  close(fd);
  return ret;
}

/****************************************************************************
 * Name: w25_crc24
 *
 * Description:
 *   Calculate a CRC24 checksum on the data loaded into memory starting at
 *   PROGSTART.
 *
 ****************************************************************************/

uint24_t w25_crc24(uint32_t len)
{
  FAR const uint8_t *src = (FAR const uint8_t *)PROGSTART;
  uint32_t crc = 0;
  int i;
  int j;

  for (i = 0; i < len; i++)
    {
      uint8_t val = *src++;
      crc ^= (uint32_t)val << 16;

      for (j = 0; j < 8; j++)
        {
          crc <<= 1;
          if ((crc & 0x1000000) != 0)
            {
              crc ^= 0x1864cfb;
            }
        }
    }

  return (uint24_t)crc;
}

/****************************************************************************
 * Name: w25_read_verify
 *
 * Description:
 *   Verify that the program in the first partition of the W25 part matches
 *   the program in memory at address PROGSTART.
 *
 ****************************************************************************/

static int w25_read_verify(void)
{
  struct prog_header_s hdr;
  uint24_t crc;
  int fd;
  int ret;

  /* Read the program image into memory */

  ret = w25_read_binary(&hdr);
  if (ret < 0)
    {
      ret = -get_errno();
      fprintf(stderr, "ERROR: Failed to read binary: %d\n", ret);
      return ret;
    }

  printf("Verifying %lu bytes in the W25 Serial FLASH\n",
         (unsigned long)hdr.len);

  /* Compare CRCs */

  crc = w25_crc24(hdr.len);
  if (crc == hdr.crc)
    {
      printf("Successfully verified %lu bytes in the W25 Serial FLASH\n",
             (unsigned long)hdr.len);
    }
  else
    {
      fprintf(stderr, "ERROR: CRC check failed\n");
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: w25_main
 *
 * Description:
 *   w25_main is a tiny program that runs in ISRAM.  w25_main will
 *   configure DRAM and the W25 serial FLASH, present a prompt, and load
 *   an Intel HEX file into the W25 serial FLASH (after first buffering
 *   the binary data into memory).  On re-boot, the program loaded into
 *   the W25 FLASH should then execute.
 *
 *   On entry:
 *   - SDRAM has already been initialized (we are using it for .bss!)
 *   - SPI0 chip select has already been configured.
 *
 ****************************************************************************/

int w25_main(int argc, char *argv)
{
  struct prog_header_s hdr;
  int ret;

#ifndef CONFIG_BOARD_LATE_INITIALIZE
  /* Initialize the board.  We can do this with a direct call because we are
   * a kernel thread.
   */

  ret = ez80_bringup();
  if (ret < 0)
    {
      fprintf(stderr, "ERROR: Failed initialize system: %d\n", ret);
      return EXIT_FAILURE;
    }
#endif

  for (; ; )
    {
      /* Read the HEX data into RAM */

      memset(&hdr, 0, sizeof(struct prog_header_s));
      hdr.magic = PROG_MAGIC;

      ret = w25_read_hex(&hdr.len);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: Failed to load HEX: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* Calculate a CRC24 checksum */

      hdr.crc = w25_crc24(hdr.len);

      /* The HEX file load was successful, write the data to FLASH */

      ret = w25_write_binary(&hdr);
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: Failed to write to W25: %d\n", ret);
          return EXIT_FAILURE;
        }

      /* Now verify that the image in memory and the image in FLASH are
       * truly the same.
       */

      ret = w25_read_verify();
      if (ret < 0)
        {
          fprintf(stderr, "ERROR: Failed to verify program: %d\n", ret);
          return EXIT_FAILURE;
        }
    }

  return EXIT_SUCCESS;
}
