/*
 *
 * Copyright (c) 2019, Baozuo Zuo, <baozhu.zuo@gmail.com>
 * The MIT License (MIT)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


/****************************************************************************
 * Included Files
 ****************************************************************************/



#include <crc16.h>
#include <crc32.h>

#include "zm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Paragraph 8.4.  Session Abort Sequence
 *
 * "If the receiver is receiving data in streaming mode, the Attn sequence
 *  is executed to interrupt data transmission before the Cancel sequence is
 *  sent.  The Cancel sequence consists of eight CAN characters and ten
 *  backspace characters.  ZMODEM only requires five Cancel characters, the
 *  other three are "insurance".
 *
 * "The trailing backspace characters attempt to erase the effects of the
 *  CAN characters if they are received by a command interpreter.
 */

#define CANISTR_SIZE (8+10)

const uint8_t g_canistr[CANISTR_SIZE] =
{
  /* Eight CAN characters */

  ASCII_CAN, ASCII_CAN, ASCII_CAN, ASCII_CAN, ASCII_CAN, ASCII_CAN,
  ASCII_CAN, ASCII_CAN,

  /* Ten backspace characters */

  ASCII_BS,  ASCII_BS,  ASCII_BS,  ASCII_BS,  ASCII_BS,  ASCII_BS,
  ASCII_BS,  ASCII_BS,  ASCII_BS,  ASCII_BS
};

/****************************************************************************
 * Public Function Protypes
 ****************************************************************************/

/****************************************************************************
 * Name: zm_putzdle
 *
 * Description:
 *   Transfer a value to a buffer performing ZDLE escaping if necessary.
 *
 * Input Parameters:
 *   pzm - Zmodem session state
 *   buffer - Buffer in which to add the possibly escaped character
 *   ch - The raw, unescaped character to be added
 *
 ****************************************************************************/

uint8_t *zm_putzdle(struct zm_state_s *pzm, uint8_t *buffer,
                        uint8_t ch)
{
  uint8_t ch7 = ch & 0x7f;

  /* Check if this character requires ZDLE escaping.
   *
   * The Zmodem protocol requires that CAN(ZDLE), DLE, XON, XOFF and a CR
   * following '@' be escaped.
   */

  if (ch   == ZDLE ||
      ch7  == ASCII_DLE ||
      ch7  == ASCII_DC1 ||
      ch7  == ASCII_DC3 ||
      ch7  == ASCII_GS ||
      (ch7 == '\r' && (pzm->flags & ZM_FLAG_ATSIGN)  != 0) ||
#ifndef CONFIG_SYSTEM_ZMODEM_DISABLE_ZRUB
      (ch7 <  ' '  && (pzm->flags & ZM_FLAG_ESCCTRL) != 0) ||
      ch7  == ASCII_DEL ||
      ch   == 0xff
#else
      (ch7 <  ' '  && (pzm->flags & ZM_FLAG_ESCCTRL) != 0)
#endif /* CONFIG_SYSTEM_ZMODEM_DISABLE_ZRUB */
      )
    {
      /* Yes... save the data link escape the character */

      *buffer++ = ZDLE;

      /* And modify the character itself as appropriate  */

      if (ch == ASCII_DEL)
        {
          ch = ZRUB0;
        }
      else if (ch == 0xff)
        {
          ch = ZRUB1;
        }
      else
        {
          ch ^= 0x40;
        }
    }

  /* Save the possibly escaped character */

  *buffer++ = ch;

  /* Check if the character is the AT sign */

  if (ch7 == '@')
    {
      pzm->flags |= ZM_FLAG_ATSIGN;
    }
  else
    {
      pzm->flags &= ~ZM_FLAG_ATSIGN;
    }

  return buffer;
}

/****************************************************************************
 * Name: zm_senddata
 *
 * Description:
 *   Send data to the remote peer performing CRC operations as required
 *   (ZBIN or ZBIN32 format assumed, ZCRCW terminator is always used)
 *
 * Input Parameters:
 *   pzm    - Zmodem session state
 *   buffer - Buffer of data to be sent
 *   buflen - The number of bytes in buffer to be sent
 *
 ****************************************************************************/

int zm_senddata(struct zm_state_s *pzm, const uint8_t *buffer,
                size_t buflen)
{
  uint8_t *ptr = pzm->scratch;
  ssize_t nwritten;
  uint32_t crc;
  uint8_t zbin;
  uint8_t term;
  int i;

  /* Make select ZBIN or ZBIN32 format and the ZCRCW terminator */

  if ((pzm->flags & ZM_FLAG_CRC32) != 0)
    {
      zbin = ZBIN32;
      crc  = 0xffffffff;
    }
  else
    {
      zbin = ZBIN;
      crc  = 0;
    }

  term = ZCRCW;
  zmdbg("zbin=%c, buflen=%ld, term=%c flags=%04x\n",
        zbin, buflen, term, pzm->flags);

  /* Transfer the data to the I/O buffer, accumulating the CRC */

  while (buflen-- > 0)
    {
      if (zbin == ZBIN)
        {
          crc = (uint32_t)crc16part(buffer, 1, (uint16_t)crc);
        }
      else /* zbin = ZBIN32 */
        {
          crc = crc32part(buffer, 1, crc);
        }

      ptr = zm_putzdle(pzm, ptr, *buffer++);
    }

  /* Trasnfer the data link escape character (without updating the CRC) */

  *ptr++ = ZDLE;

  /* Transfer the terminating character, updating the CRC */

  if (zbin == ZBIN)
    {
      crc = (uint32_t)crc16part((const uint8_t *)&term, 1, (uint16_t)crc);
    }
  else
    {
      crc = crc32part((const uint8_t *)&term, 1, crc);
    }

  *ptr++ = term;

  /* Calcualate and transfer the final CRC value */

  if (zbin == ZBIN)
    {
      //crc = (uint32_t)crc16part(g_zeroes, 2, (uint16_t)crc);
      ptr = zm_putzdle(pzm, ptr, (crc >> 8) & 0xff);
      ptr = zm_putzdle(pzm, ptr, crc & 0xff);
    }
  else
    {
      crc = ~crc;
      for (i = 0; i < 4; i++, crc >>= 8)
        {
          ptr = zm_putzdle(pzm, ptr, crc & 0xff);
        }
    }

  /* Send the header */
  nwritten = zm_remwrite(pzm, pzm->scratch, ptr - pzm->scratch);
  return nwritten < 0 ? (int)nwritten : 0;
}

/****************************************************************************
 * Name: zm_sendhexhdr
 *
 * Description:
 *   Send a ZHEX header to the remote peer performing CRC operations as
 *   necessary.
 *
 *   Hex header:
 *     ZPAD ZPAD ZDLE ZHEX type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2 CR LF [XON]
 *     Payload length: 16 (14 hex digits, cr, lf, ignoring optional XON)
 *
 * Input Parameters:
 *   pzm    - Zmodem session state
 *   type   - Header type {ZRINIT, ZRQINIT, ZDATA, ZACK, ZNAK, ZCRC, ZRPOS,
 *            ZCOMPL, ZEOF, ZFIN}
 *   buffer - 4-byte buffer of data to be sent
 *
 * Assumptions:
 *   The allocated I/O buffer is available to buffer file data.
 *
 ****************************************************************************/

int zm_sendhexhdr(struct zm_state_s *pzm, int type,
                  const uint8_t *buffer)
{
  uint8_t *ptr;
  ssize_t nwritten;
  uint16_t crc;
  int i;

  zmdbg("Sending type %d: %02x %02x %02x %02x\n",
       type, buffer[0], buffer[1], buffer[2], buffer[3]);

  /* ZPAD ZPAD ZDLE ZHEX */

  ptr = pzm->scratch;
  *ptr++ = ZPAD;
  *ptr++ = ZPAD;
  *ptr++ = ZDLE;
  *ptr++ = ZHEX;

  /* type */

  crc = crc16part((const uint8_t *)&type, 1, 0);
  ptr = zm_puthex8(ptr, type);

  /* f3/p0 f2/p1 f1/p2 f0/p3 */

  crc = crc16part(buffer, 4, crc);
  for (i = 0; i < 4; i++)
    {
      ptr = zm_puthex8(ptr, *buffer++);
    }

  /* crc-1 crc-2 */

  crc = crc16part(g_zeroes, 2, crc);
  ptr = zm_puthex8(ptr, (crc >> 8) & 0xff);
  ptr = zm_puthex8(ptr, crc & 0xff);

  /* CR LF */

  *ptr++ = '\r';
  *ptr++ = '\n';

  /* [XON] */

  if (type != ZACK && type != ZFIN)
    {
      *ptr++ = ASCII_XON;
    }

  /* Send the header */
  nwritten = pzm->write(pzm->arg, pzm->scratch, ptr - pzm->scratch);
  return nwritten < 0 ? (int)nwritten : 0;
}

/****************************************************************************
 * Name: zm_sendbin16hdr
 *
 * Description:
 *   Send a ZBIN header to the remote peer performing CRC operations as
 *   necessary.  Normally called indirectly through zm_sendbinhdr().
 *
 *   16-bit binary header:
 *     ZPAD ZDLE ZBIN type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2
 *     Payload length: 7 (type, 4 bytes data, 2 byte CRC)
 *
 * Input Parameters:
 *   pzm    - Zmodem session state
 *   type   - Header type {ZSINIT, ZFILE, ZDATA, ZDATA}
 *   buffer - 4-byte buffer of data to be sent
 *
 * Assumptions:
 *   The allocated I/O buffer is available to buffer file data.
 *
 ****************************************************************************/

int zm_sendbin16hdr(struct zm_state_s *pzm, int type,
                    const uint8_t *buffer)
{
  uint8_t *ptr;
  ssize_t nwritten;
  uint16_t crc;
  int buflen;
  int i;

  zmdbg("Sending type %d: %02x %02x %02x %02x\n",
       type, buffer[0], buffer[1], buffer[2], buffer[3]);

  /* XPAD ZDLE ZBIN */

  ptr    = pzm->scratch;
  *ptr++ = ZPAD;
  *ptr++ = ZDLE;
  *ptr++ = ZBIN;

  /* type */

  crc = crc16part((const uint8_t *)&type, 1, 0);
  ptr = zm_putzdle(pzm, ptr, type);

  /* f3/p0 f2/p1 f1/p2 f0/p3 */

  crc = crc16part(buffer, 4, crc);
  for (i = 0; i < 4; i++)
    {
      ptr = zm_putzdle(pzm, ptr, *buffer++);
    }

  /* crc-1 crc-2 */

  //crc = crc16part(g_zeroes, 2, crc);
  ptr = zm_putzdle(pzm, ptr, (crc >> 8) & 0xff);
  ptr = zm_putzdle(pzm, ptr, crc & 0xff);

  /* Send the header */

  buflen   = ptr - pzm->scratch;
  nwritten = zm_remwrite(pzm->arg, pzm->scratch, buflen);
  return nwritten < 0 ? (int)nwritten : 0;
}

/****************************************************************************
 * Name: zm_sendbin32hdr
 *
 * Description:
 *   Send a ZBIN32 header to the remote peer performing CRC operations as
 *   necessary.  Normally called indirectly through zm_sendbinhdr().
 *
 *   32-bit inary header:
 *     ZPAD ZDLE ZBIN32 type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2 crc-3 crc-4
 *     Payload length: 9 (type, 4 bytes data, 4 byte CRC)
 *
 * Input Parameters:
 *   pzm    - Zmodem session state
 *   type   - Header type {ZSINIT, ZFILE, ZDATA, ZDATA}
 *   buffer - 4-byte buffer of data to be sent
 *
 * Assumptions:
 *   The allocated I/O buffer is available to buffer file data.
 *
 ****************************************************************************/

int zm_sendbin32hdr(struct zm_state_s *pzm, int type,
                    const uint8_t *buffer)
{
  uint8_t *ptr;
  ssize_t nwritten;
  uint32_t crc;
  int buflen;
  int i;

  zmdbg("Sending  type %d: %02x %02x %02x %02x\n",
        type, buffer[0], buffer[1], buffer[2], buffer[3]);

  /* XPAD ZDLE ZBIN32 */

  ptr = pzm->scratch;
  *ptr++ = ZPAD;
  *ptr++ = ZDLE;
  *ptr++ = ZBIN32;

  /* type */

  ptr = zm_putzdle(pzm, ptr, type);
  crc = crc32part((const uint8_t *)&type, 1, 0xffffffffL);

  /* f3/p0 f2/p1 f1/p2 f0/p3 */

  crc = crc32part(buffer, 4, crc);
  for (i = 0; i < 4; i++)
    {
      ptr = zm_putzdle(pzm, ptr, *buffer++);
    }

  /* crc-1 crc-2 crc-3 crc-4 */

  crc = ~crc;
  for (i = 0; i < 4; i++, crc >>= 8)
    {
      ptr = zm_putzdle(pzm, ptr, crc & 0xff);
    }

  /* Send the header */

  buflen   = ptr - pzm->scratch;
  nwritten = zm_remwrite(pzm, pzm->scratch, buflen);
  return nwritten < 0 ? (int)nwritten : 0;
}

/****************************************************************************
 * Name: zm_sendbinhdr
 *
 * Description:
 *   Send a binary header to the remote peer.  This is a simple wrapper
 *   function for zm_sendbin16hdr() and zm_sendbin32hdr().  It decides on
 *   the correct CRC format and re-directs the call appropriately.
 *
 * Input Parameters:
 *   pzm    - Zmodem session state
 *   type   - Header type {ZSINIT, ZFILE, ZDATA, ZDATA}
 *   buffer - 4-byte buffer of data to be sent
 *
 * Assumptions:
 *   The allocated I/O buffer is available to buffer file data.
 *
 ****************************************************************************/

int zm_sendbinhdr(struct zm_state_s *pzm, int type,
                  const uint8_t *buffer)
{
  if ((pzm->flags & ZM_FLAG_CRC32) == 0)
    {
      return zm_sendbin16hdr(pzm, type, buffer);
    }
  else
    {
      return zm_sendbin32hdr(pzm, type, buffer);
    }
}
