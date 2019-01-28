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

/* Zmodem receive states.
 *
 * A simple transaction, one file, no errors, no CHALLENGE, overlapped I/O:
 * These happen when zm_read() is called:
 *
 *   Sender               Receiver    State
 *   --------------     ------------  --------
 *   "rz\r"       ---->
 *   ZRQINIT      ---->
 *                <---- ZRINIT        ZMR_START
 *   ZSINIT       ---->
 *                <---- ZACK          ZMR_INITWAIT
 *   ZFILE        ---->
 *                <---- ZRPOS         ZMR_FILEINFO
 *   ZDATA        ---->
 *                <---- ZCRC          ZMR_CRCWAIT
 *   ZCRC         ---->               ZMR_READREADY
 *   Data packets ---->               ZMR_READING
 *   Last packet  ---->
 *   ZEOF         ---->
 *                <---- ZRINIT
 *   ZFIN         ---->
 *                <---- ZFIN          ZMR_FINISH
 *   OO           ---->               ZMR_DONE
 */
#include "zm.h"
#include "zmodem.h"



enum zmrs_e
{
  ZMR_START = 0,   /* Sent ZRINIT, waiting for ZFILE or ZSINIT */
  ZMR_INITWAIT,    /* Received ZSINIT, sent ZACK, waiting for ZFILE */
  ZMR_FILEINFO,    /* Received ZFILE, sent ZRPOS, waiting for filename in ZDATA */
  ZMR_CRCWAIT,     /* Received ZDATA filename, send ZCRC, wait for ZCRC response */
  ZMR_READREADY,   /* Received ZDATA filename and ZCRC, ready for data packets */
  ZMR_READING,     /* Reading data */
  ZMR_FINISH,      /* Received ZFIN, sent ZFIN, waiting for "OO" or ZRQINIT */
  ZMR_COMMAND,     /* Waiting for command data */
  ZMR_MESSAGE,     /* Waiting for message from receiver */
  ZMR_DONE         /* Finished with transfer */
};

static const struct zm_transition_s *g_zmr_evtable[] =
{
  // g_zmr_start,     /* ZMR_START:     Sent ZRINIT, waiting for ZFILE or ZSINIT */
  // g_zmr_initwait,  /* ZMR_INITWAIT:  Received ZSINIT, sent ZACK, waiting for ZFILE */
  // g_zmr_fileinfo,  /* XMRS_FILENAME: Received ZFILE, sent ZRPOS, waiting for ZDATA */
  // g_zmr_crcwait,   /* ZMR_CRCWAIT:   Received ZDATA, send ZCRC, wait for ZCRC */
  // g_zmr_readready, /* ZMR_READREADY: Received ZCRC, ready for data packets */
  // g_zmr_reading,   /* ZMR_READING:   Reading data */
  // g_zmr_finish,    /* ZMR_FINISH:    Sent ZFIN, waiting for "OO" or ZRQINIT */
  // g_zmr_command,   /* ZMR_COMMAND:   Waiting for command data */
  // g_zmr_message,   /* ZMR_MESSAGE:   Receiver wants to print a message */
  // g_zmr_done       /* ZMR_DONE:      Transfer is complete */
};

/*
 * Private Functions
 */

/*
 * Name: zmr_initialize
 *
 * Description:
 *   Initialize for Zmodem receive operation
 * 
 * Returned Value:
 *   An opaque handle that can be use with zmr_receive() and zmr_release().
 *
 */

struct zmr_state_s * zmr_initialize(size_t (*write)(const uint8_t *buffer, size_t buflen), 
                                    size_t (*read)(const uint8_t *buffer, size_t buflen) ){
  struct zmr_state_s *pzmr;
  /* Allocate a new Zmodem receive state structure */

  pzmr = (struct zmr_state_s*)malloc(sizeof(struct zmr_state_s));
  if (NULL == pzmr){
      zmdbg("ERROR: zmr_state_s failed!\r\n");
    }

  pzmr->cmn.write = write;
  pzmr->cmn.read = read;

  return pzmr;
}

/*
 * Name: zmr_zrinit
 *
 * Description:
 *   Resend ZRINIT header in response to ZRQINIT or ZNAK header
 *
 *   Paragraph 9.5 "If the receiver cannot overlap serial and disk I/O, it
 *   uses the ZRINIT frame to specify a buffer length which the sender will
 *   not overflow.  The sending program sends a ZCRCW data subpacket and
 *   waits for a ZACK header before sending the next segment of the file."
 *
 */

static int zmr_zrinit(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;
  uint8_t buf[4];

  zmdbg("ZMR_STATE %d:->%d Send ZRINIT\n", pzm->state, ZMR_START);
  pzm->state   = ZMR_START;
  pzm->flags  &= ~ZM_FLAG_OO;   /* In case we get here from ZMR_FINISH */

  /* Send ZRINIT */

  pzm->timeout = CONFIG_SYSTEM_ZMODEM_RESPTIME;
  buf[0]       = CONFIG_SYSTEM_ZMODEM_PKTBUFSIZE & 0xff;
  buf[1]       = (CONFIG_SYSTEM_ZMODEM_PKTBUFSIZE >> 8) & 0xff;
  buf[2]       = 0;
  buf[3]       = pzmr->rcaps;
  return zm_sendhexhdr(pzm, ZRINIT, buf);
}