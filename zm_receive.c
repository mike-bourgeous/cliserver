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

#include <string.h>

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

/*
 * Private Function Prototypes
 */
/* Transition actions */

static int zmr_zrinit(struct zm_state_s *pzm);
static int zmr_zsinit(struct zm_state_s *pzm);
static int zmr_zsrintdata(struct zm_state_s *pzm);
static int zmr_startto(struct zm_state_s *pzm);
static int zmr_freecnt(struct zm_state_s *pzm);
static int zmr_zcrc(struct zm_state_s *pzm);
static int zmr_nakcrc(struct zm_state_s *pzm);
static int zmr_zfile(struct zm_state_s *pzm);
static int zmr_zdata(struct zm_state_s *pzm);
static int zmr_badrpos(struct zm_state_s *pzm);
static int zmr_filename(struct zm_state_s *pzm);
static int zmr_filedata(struct zm_state_s *pzm);
static int zmr_rcvto(struct zm_state_s *pzm);
static int zmr_fileto(struct zm_state_s *pzm);
static int zmr_cmddata(struct zm_state_s *pzm);
static int zmr_zeof(struct zm_state_s *pzm);
static int zmr_zfin(struct zm_state_s *pzm);
static int zmr_finto(struct zm_state_s *pzm);
static int zmr_oo(struct zm_state_s *pzm);
static int zmr_message(struct zm_state_s *pzm);
static int zmr_zstderr(struct zm_state_s *pzm);
static int zmr_cmdto(struct zm_state_s *pzm);
static int zmr_doneto(struct zm_state_s *pzm);
static int zmr_error(struct zm_state_s *pzm);

/* Internal helpers */

// static int zmr_parsefilename(struct zmr_state_s *pzmr,
//                              const uint8_t *namptr);
static int zmr_openfile(struct zmr_state_s *pzmr, uint32_t crc);
static int zmr_fileerror(struct zmr_state_s *pzmr, uint8_t type,
                         uint32_t data);
static void zmr_filecleanup(struct zmr_state_s *pzmr);

/*
 * Private Data
 */

/* Events handled in state ZMR_START - Sent ZRINIT, waiting for ZFILE or
 * ZSINIT
 */

static const struct zm_transition_s g_zmr_start[] =
{
  {ZME_SINIT,    false, ZMR_INITWAIT,    zmr_zsinit},
  {ZME_FILE,     false, ZMR_FILEINFO,    zmr_zfile},
  {ZME_RQINIT,   false, ZMR_START,       zmr_zrinit},
  {ZME_FIN,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_NAK,      true,  ZMR_START,       zmr_zrinit},
  {ZME_FREECNT,  false, ZMR_START,       zmr_freecnt},
  {ZME_COMMAND,  false, ZMR_COMMAND,     zmr_cmddata},
  {ZME_STDERR,   false, ZMR_MESSAGE,     zmr_message},
  {ZME_TIMEOUT,  false, ZMR_START,       zmr_startto},
  {ZME_ERROR,    false, ZMR_START,       zmr_error}
};

/* Events handled in state ZMR_INITWAIT - Received ZSINIT, sent ZACK,
 * waiting for ZFILE.
 */

static const struct zm_transition_s g_zmr_initwait[] =
{
  {ZME_DATARCVD, false, ZMR_START,       zmr_zsrintdata},
  {ZME_TIMEOUT,  false, ZMR_INITWAIT,    zmr_rcvto},
  {ZME_ERROR,    false, ZMR_INITWAIT,    zmr_error}
};

/* Events handled in state ZMR_FILEINFO - eceived ZFILE, sent ZRPOS, waiting
 * for filename in ZDATA
 */

static const struct zm_transition_s g_zmr_fileinfo[] =
{
  {ZME_DATARCVD, false, ZMR_READREADY,   zmr_filename},
  {ZME_TIMEOUT,  false, ZMR_FILEINFO,    zmr_rcvto},
  {ZME_ERROR,    false, ZMR_FILEINFO,    zmr_error}
};

/* Events handled in state ZMR_CRCWAIT - Received ZDATA filename, send ZCRC,
 * wait for ZCRC response
 */

static const struct zm_transition_s g_zmr_crcwait[] =
{
  {ZME_CRC,      false, ZMR_READREADY,   zmr_zcrc},
  {ZME_NAK,      false, ZMR_CRCWAIT,     zmr_nakcrc},
  {ZME_RQINIT,   true,  ZMR_START,       zmr_zrinit},
  {ZME_FIN,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_TIMEOUT,  false, ZMR_CRCWAIT,     zmr_fileto},
  {ZME_ERROR,    false, ZMR_CRCWAIT,     zmr_error}
};

/* Events handled in state ZMR_READREADY - Received ZDATA filename and ZCRC,
 * ready for data packets
 */

static const struct zm_transition_s g_zmr_readready[] =
{
  {ZME_DATA,     false, ZMR_READING,     zmr_zdata},
  {ZME_NAK,      false, ZMR_READREADY,   zmr_badrpos},
  {ZME_EOF,      false, ZMR_START,       zmr_zeof},
  {ZME_RQINIT,   true,  ZMR_START,       zmr_zrinit},
  {ZME_FILE,     false, ZMR_READREADY,   zmr_badrpos},
  {ZME_FIN,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_TIMEOUT,  false, ZMR_READREADY,   zmr_fileto},
  {ZME_ERROR,    false, ZMR_READREADY,   zmr_error}
};

/* Events handled in the state ZMR_READING - Reading data */

static const struct zm_transition_s g_zmr_reading[] =
{
  {ZME_RQINIT,   true,  ZMR_START,       zmr_zrinit},
  {ZME_FILE,     false, ZMR_FILEINFO,    zmr_zfile},
  {ZME_NAK,      true,  ZMR_READREADY,   zmr_badrpos},
  {ZME_FIN,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_DATA,     false, ZMR_READING,     zmr_zdata},
  {ZME_EOF,      true,  ZMR_START,       zmr_zeof},
  {ZME_DATARCVD, false, ZMR_READING,     zmr_filedata},
  {ZME_TIMEOUT,  false, ZMR_READING,     zmr_fileto},
  {ZME_ERROR,    false, ZMR_READING,     zmr_error}
};

/* Events handled in the state ZMR_FINISH - Sent ZFIN, waiting for "OO" or ZRQINIT */

static const struct zm_transition_s g_zmr_finish[] =
{
  {ZME_RQINIT,   true,  ZMR_START,       zmr_zrinit},
  {ZME_FILE,     true,  ZMR_FILEINFO,    zmr_zfile},
  {ZME_NAK,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_FIN,      true,  ZMR_FINISH,      zmr_zfin},
  {ZME_TIMEOUT,  false, ZMR_READING,     zmr_finto},
  {ZME_OO,       false, ZMR_READING,     zmr_oo},
  {ZME_ERROR,    false, ZMR_FINISH,      zmr_error}
};

/* Events handled in the state ZMR_COMMAND -  Waiting for command data */

static const struct zm_transition_s g_zmr_command[] =
{
  {ZME_DATARCVD, false, ZMR_COMMAND,     zmr_cmddata},
  {ZME_TIMEOUT,  false, ZMR_COMMAND,     zmr_cmdto},
  {ZME_ERROR,    false, ZMR_COMMAND,     zmr_error}
};

/* Events handled in ZMR_MESSAGE - Waiting for ZSTDERR data */

static struct zm_transition_s g_zmr_message[] =
{
  {ZME_DATARCVD, false, ZMR_MESSAGE,     zmr_zstderr},
  {ZME_TIMEOUT,  false, ZMR_MESSAGE,     zmr_cmdto},
  {ZME_ERROR,    false, ZMR_MESSAGE,     zmr_error}
};

/* Events handled in ZMR_DONE -- Finished with transfer.  Waiting for "OO" or  */

static struct zm_transition_s g_zmr_done[] =
{
  {ZME_TIMEOUT,  false, ZMR_DONE,        zmr_doneto},
  {ZME_ERROR,    false, ZMR_DONE,        zmr_error}
};

/* State x Event table for Zmodem receive.  The order of states must
 * exactly match the order defined in enum zmrs_e
 */

static const struct zm_transition_s *g_zmr_evtable[] =
{
  g_zmr_start,     /* ZMR_START:     Sent ZRINIT, waiting for ZFILE or ZSINIT */
  g_zmr_initwait,  /* ZMR_INITWAIT:  Received ZSINIT, sent ZACK, waiting for ZFILE */
  g_zmr_fileinfo,  /* XMRS_FILENAME: Received ZFILE, sent ZRPOS, waiting for ZDATA */
  g_zmr_crcwait,   /* ZMR_CRCWAIT:   Received ZDATA, send ZCRC, wait for ZCRC */
  g_zmr_readready, /* ZMR_READREADY: Received ZCRC, ready for data packets */
  g_zmr_reading,   /* ZMR_READING:   Reading data */
  g_zmr_finish,    /* ZMR_FINISH:    Sent ZFIN, waiting for "OO" or ZRQINIT */
  g_zmr_command,   /* ZMR_COMMAND:   Waiting for command data */
  g_zmr_message,   /* ZMR_MESSAGE:   Receiver wants to print a message */
  g_zmr_done       /* ZMR_DONE:      Transfer is complete */
};

/*
 * Private Functions
 */

/*
 * Name: zm_readstate
 *
 * Description:
 *   Enter PSTATE_DATA.
 *
 */

void zm_readstate(struct zm_state_s *pzm)
{
  zmdbg("PSTATE %d:%d->%d:%d\n",
        pzm->pstate, pzm->psubstate, PSTATE_DATA, PDATA_READ);

  pzm->pstate    = PSTATE_DATA;
  pzm->psubstate = PDATA_READ;
  pzm->pktlen    = 0;
  pzm->ncrc      = 0;
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

/*
 * Name: zmr_zsinit
 *
 * Description:
 *   Received a ZSINIT header in response to ZRINIT
 *
 */

static int zmr_zsinit(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  zmdbg("ZMR_STATE %d: Received ZSINIT header\n", pzm->state);

  /* Get the sender's capabilities */

  pzmr->scaps = pzm->hdrdata[4];

  /* Does the sender expect control characters to be escaped? */

  pzm->flags &= ~ZM_FLAG_ESCCTRL;
  if ((pzmr->scaps & TESCCTL) != 0)
    {
      pzm->flags |= ZM_FLAG_ESCCTRL;
    }

  /* Setup to receive a data packet.  Enter PSTATE_DATA */

  zm_readstate(pzm);
  return 0;
}

/*
 * Name: zmr_startto
 *
 * Description:
 *   Timed out waiting for ZSINIT or ZFILE.
 *
 */

static int zmr_startto(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  pzmr->ntimeouts++;
  zmdbg("ZMR_STATE %d: %d timeouts waiting for ZSINIT or ZFILE\n",
        pzm->state, pzmr->ntimeouts);

  if (pzmr->ntimeouts > 4)
    {
      /* Send ZRINIT again */

      return zmr_zrinit(pzm);
    }

  /* This will stop the file transfer */

  return -ETIMEDOUT;
}

/*
 * Name: zmr_zsrintdata
 *
 * Description:
 *   Received the rest of the ZSINIT packet.
 *
 */

static int zmr_zsrintdata(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;
  uint8_t by[4];

  zmdbg("PSTATE %d:%d->%d:%d. Received the rest of the ZSINIT packet\n",
        pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);

  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* NAK if the CRC was bad */

  if ((pzm->flags & ZM_FLAG_CRKOK) == 0)
    {
      return zm_sendhexhdr(pzm, ZNAK, g_zeroes);
    }

  /* Release any previously allocated attention strings */

  if (pzmr->attn != NULL)
    {
      free(pzmr->attn);
    }

  /* Get the new attention string */

  pzmr->attn = NULL;
  if (pzm->pktbuf[0] != '\0')
    {
      pzmr->attn = strdup((char *)pzm->pktbuf);
    }

  /* And send ZACK */

  zm_be32toby(CONFIG_SYSTEM_ZMODEM_SERIALNO, by);
  return zm_sendhexhdr(pzm, ZACK, by);
}

/*
 * Name: zmr_freecnt
 *
 * Description:
 *   TODO: This is supposed to return the amount of free space on the media.
 *   We just return a really big number now.
 *
 */

static int zmr_freecnt(struct zm_state_s *pzm)
{
  uint8_t by[4];

  zmdbg("ZMR_STATE %d\n", pzm->state);

  zm_be32toby(0xffffffff, by);
  return zm_sendhexhdr(pzm, ZACK, by);
}

/*
 * Name: zmr_zcrc
 *
 * Description:
 *   Received file CRC.  Need to accept or reject it.
 *
 */

static int zmr_zcrc(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  /* Get the remote file CRC */

  pzmr->crc = zm_bytobe32(pzm->hdrdata);

  /* And create the local file */

  zmdbg("ZMR_STATE %d: CRC=%08x call zmr_openfile\n", pzmr->crc, pzm->state);
  return zmr_openfile(pzmr, zm_bytobe32(pzm->hdrdata + 1));
}

/*
 * Name: zmr_nakcrc
 *
 * Description:
 *   The sender responded to ZCRC with NAK.  Resend the ZCRC.
 *
 */

static int zmr_nakcrc(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d: Send ZCRC\n", pzm->state);
  return zm_sendhexhdr(pzm, ZCRC, g_zeroes);
}

/*
 * Name: zmr_zfile
 *
 * Description:
 *   Received ZFILE.  Cache the flags and set up to receive filename in ZDATA.
 *
 */

static int zmr_zfile(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  zmdbg("ZMR_STATE %d\n", pzm->state);

  pzm->nerrors = 0;
  pzm->flags  &= ~ZM_FLAG_OO;   /* In case we get here from ZMR_FINISH */

  /* Cache flags (skipping of the initial header type byte) */

  pzmr->f0 = pzmr->cmn.hdrdata[4];
  pzmr->f1 = pzmr->cmn.hdrdata[3];
#if 0 /* Not used */
  pzmr->f2 = pzmr->cmn.hdrdata[2];
  pzmr->f3 = pzmr->cmn.hdrdata[1];
#endif

  /* Setup to receive a data packet.  Enter PSTATE_DATA */

  zm_readstate(pzm);
  return 0;
}

/*
 * Name: zmr_zdata
 *
 * Description:
 *   Received ZDATA header
 *
 */

static int zmr_zdata(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  zmdbg("ZMR_STATE %d\n", pzm->state);

  /* Paragraph 8.2: "The receiver compares the file position in the ZDATA
   * header with the number of characters successfully received to the file.
   * If they do not agree, a ZRPOS error response is generated to force the
   * sender to the right position within the file."
   */

  if (zm_bytobe32(pzm->hdrdata + 1) != pzmr->offset)
    {
      /* Execute the Attn sequence and then send a ZRPOS header with the
       * correct position within the file.
       */

       zmdbg("Bad position, send ZRPOS(%ld)\n",
             (unsigned long)pzmr->offset);

       return zmr_fileerror(pzmr, ZRPOS, (uint32_t)pzmr->offset);
   }

  /* Setup to receive a data packet.  Enter PSTATE_DATA */

  zm_readstate(pzm);
  return 0;
}

/*
 * Name: zmr_badrpos
 *
 * Description:
 *   Last ZRPOS was bad, resend it
 *
 */

static int zmr_badrpos(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;
  uint8_t by[4];

  zmdbg("ZMR_STATE %d: Send ZRPOS(%ld)\n", pzm->state, (unsigned long)pzmr->offset);

  /* Re-send ZRPOS */

  zm_be32toby(pzmr->offset, by);
  return zm_sendhexhdr(pzm, ZRPOS, by);
}

/*
 * Name: zmr_filename
 *
 * Description:
 *   Received file information
 *
 */

static int zmr_filename(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;
  const uint8_t *pktptr;
  unsigned long filesize;
  unsigned long timestamp;
  unsigned long bremaining;
  int mode;
  int serialno;
  int fremaining;
  int filetype;

  zmdbg("PSTATE %d:%d->%d:%d\n",
        pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);
  zmdbg("ZMR_STATE %d\n", pzm->state);

  /* Back to the IDLE state */

  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* Verify that the CRC was correct */

  if ((pzm->flags & ZM_FLAG_CRKOK) == 0)
    {
      zmdbg("ZMR_STATE %d->%d: ERROR: Bad CRC, send ZNAK\n",
            pzm->state, ZMR_START);

      /* Send NACK if the CRC is bad */

      pzm->state = ZMR_START;
      return zm_sendhexhdr(pzm, ZNAK, g_zeroes);
    }

  /* Discard any previous local file names */

  if (pzmr->filename != NULL)
    {
      free(pzmr->filename);
      pzmr->filename = NULL;
    }

  /* Parse the new file name from the beginning of the packet and verify
   * that we can use it.
   */

  pktptr = pzmr->cmn.pktbuf;
  // ret    = zmr_parsefilename(pzmr, pktptr);

  // if (ret < 0)
  //   {
  //     zmdbg("ZMR_STATE %d->%d: ERROR: Failed to parse filename. Send ZSKIP: %d\n",
  //           pzm->state, ZMR_START, ret);

  //     pzmr->cmn.state = ZMR_START;
  //     return zm_sendhexhdr(&pzmr->cmn, ZSKIP, g_zeroes);
  //   }

  /* Skip over the file name (and its NUL termination) */

  pktptr += (strlen((const char *)pktptr) + 1);

  /* ZFILE: Following the file name are:
   *
   *   length timestamp mode serial-number files-remaining bytes-remaining file-type
   */

  filesize   = 0;
  timestamp  = 0;
  mode       = 0;
  serialno   = 0;
  fremaining = 0;
  bremaining = 0;
  filetype   = 0;

  sscanf((char *)pktptr, "%lu %lo %o %o %d %lu %d",
         &filesize, &timestamp, &mode, &serialno, &fremaining, &bremaining,
         &filetype);

  /* Only a few of these values are retained in this implementation */

  pzmr->filesize  = (off_t)filesize;
#ifdef CONFIG_SYSTEM_ZMODEM_TIMESTAMPS
  pzmr->timestamp = (time_t)timestamp;
#endif

  /* Check if we need to send the CRC */

  if ((pzmr->f1 & ZMMASK) == ZMCRC)
    {
      zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_CRCWAIT);

      pzm->state = ZMR_CRCWAIT;
      return zm_sendhexhdr(pzm, ZCRC, g_zeroes);
    }

  /* We are ready to receive file data packets */

  zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_READREADY);

  pzm->state = ZMR_READREADY;
  return zmr_openfile(pzmr, 0);
}

/*
 * Name: zmr_filedata
 *
 * Description:
 *   Received file data
 *
 */

static int zmr_filedata(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;
  uint8_t by[4];
  int ret;

  zmdbg("ZMR_STATE %d\n", pzm->state);

  /* Check if the CRC is okay */

  if ((pzm->flags & ZM_FLAG_CRKOK) == 0)
    {
      zmdbg("ERROR: Bad crc, send ZRPOS(%ld)\n",
            (unsigned long)pzmr->offset);

      /* No.. increment the count of errors */

      pzm->nerrors++;
      zmdbg("%d data errors\n", pzm->nerrors);

      /* If the count of errors exceeds the configurable limit, then cancel
       * the transfer
       */

      if (pzm->nerrors > CONFIG_SYSTEM_ZMODEM_MAXERRORS)
        {
          zmdbg("PSTATE %d:%d->%d:%d\n",
                pzm->pstate, pzm->psubstate, PSTATE_DATA, PDATA_READ);

          /* Send the cancel string */

           pzm->write(&pzmr,g_canistr, CANISTR_SIZE);

          /* Enter PSTATE_DATA */

          zm_readstate(pzm);
          return -EIO;
        }
      else
        {
          zmdbg("PSTATE %d:%d->%d:%d\n",
                pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);
          zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_READREADY);

          /* Revert to the ready to read state and send ZRPOS to get in sync */

          pzm->state     = ZMR_READREADY;
          pzm->pstate    = PSTATE_IDLE;
          pzm->psubstate = PIDLE_ZPAD;
          return zmr_fileerror(pzmr, ZRPOS, (uint32_t)pzmr->offset);
        }
    }

  /* call the on_receive event */
  ret = pzm->on_receive(&pzmr,pzm->pktbuf, pzm->pktlen, pzmr->f0 == ZCNL);

  if (ret < 0)
    {
      int errorcode = -EPERM;

      /* Could not write to the file. */

      zmdbg("ERROR: Write to file failed: %d\n", errorcode);
      zmdbg("PSTATE %d:%d->%d:%d\n",
             pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);
      zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_FINISH);

      /* Revert to the IDLE state, send ZFERR, and terminate the transfer
       * with an error.
       */

      pzm->state     = ZMR_FINISH;
      pzm->pstate    = PSTATE_IDLE;
      pzm->psubstate = PIDLE_ZPAD;
      (void)zmr_fileerror(pzmr, ZFERR, (uint32_t)errorcode);
      return -errorcode;
    }

  zmdbg("offset: %ld nchars: %d pkttype: %02x\n",
        (unsigned long)pzmr->offset, pzm->pktlen, pzm->pkttype);

  pzmr->offset += pzm->pktlen;
  zmdbg("Bytes received: %ld\n", (unsigned long)pzmr->offset);

  /* If this was the last data subpacket, leave data mode */

  if (pzm->pkttype == ZCRCE || pzm->pkttype == ZCRCW)
    {
      zmdbg("PSTATE %d:%d->%d:%d: ZCRCE|ZCRCW\n",
            pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);
      zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_READREADY);

      /* Revert to the IDLE state */

      pzm->state     = ZMR_READREADY;
      pzm->pstate    = PSTATE_IDLE;
      pzm->psubstate = PIDLE_ZPAD;
    }
  else
    {
      /* Setup to receive a data packet.  Enter PSTATE_DATA */

      zm_readstate(pzm);
    }

  /* Special handle for different packet types:
   *
   *   ZCRCW:  Non-streaming, ZACK required
   *   ZCRCG:  Streaming, no response
   *   ZCRCQ:  Streaming, ZACK required
   *   ZCRCE:  End of file, no response
   */

  if (pzm->pkttype == ZCRCQ || pzm->pkttype == ZCRCW)
    {
      zmdbg("Send ZACK\n");

      zm_be32toby(pzmr->offset, by);
      return zm_sendhexhdr(pzm, ZACK, by);
    }

  return 0;
}

/*
 * Name: zmr_rcvto
 *
 * Description:
 *   Timed out waiting:
 *
 *   1) In state ZMR_INITWAIT - Received ZSINIT, waiting for data, or
 *   2) In state XMRS_FILENAME - Received ZFILE, waiting for file _info
 *
 */

static int zmr_rcvto(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  /* Increment the count of timeouts */

  pzmr->ntimeouts++;
  zmdbg("ZMR_STATE %d: Send timeouts: %d\n", pzm->state, pzmr->ntimeouts);

  /* If the number of timeouts exceeds a limit, then about the transfer */

  if (pzmr->ntimeouts > 4)
    {
      return -ETIMEDOUT;
    }

  /* Re-send the ZRINIT header */

  return zmr_zrinit(pzm);
}

/*
 * Name: zmr_fileto
 *
 * Description:
 *   Timed out waiting
 *   1) In state ZMR_CRCWAIT - Received filename, waiting for CRC,
 *   2) In state ZMR_READREADY - Received filename, ready to read, or
 *   3) IN state ZMR_READING - Reading data
 *
 */

static int zmr_fileto(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  /* Increment the count of timeouts */

  pzmr->ntimeouts++;
  zmdbg("ZMR_STATE %d: %d send timeouts\n", pzm->state, pzmr->ntimeouts);

  /* If the number of timeouts exceeds a limit, then restart the transfer */

  if (pzmr->ntimeouts > 2)
    {
      /* Re-send the ZRINIT header */

      pzmr->ntimeouts = 0;
      return zmr_zrinit(pzm);
    }

  return pzm->state == ZMR_CRCWAIT ? zmr_nakcrc(pzm) : zmr_badrpos(pzm);
}

/*
 * Name: zmr_zeof
 *
 * Description:
 *   Received ZEOF packet. File is now complete
 *
 */

static int zmr_zeof(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  zmdbg("ZMR_STATE %d: offset=%ld\n", pzm->state, (unsigned long)pzmr->offset);

  /* Verify the file length */

  if (zm_bytobe32(pzm->hdrdata + 1) != pzmr->offset)
    {
      zmdbg("ERROR: Bad length\n");
      zmdbg("ZMR_STATE %d->%d\n",  pzm->state, ZMR_READREADY);

      pzm->state = ZMR_READREADY;
      return 0;         /* it was probably spurious */
    }

  /* TODO:  Set the file timestamp and access privileges */

  /* Re-send the ZRINIT header so that we are ready for the next file */

  return zmr_zrinit(pzm);
}

/*
 * Name: zmr_cmddata
 *
 * Description:
 *   Received command data (not implemented)
 *
 */

static int zmr_cmddata(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d\n", pzm->state);
  return 0;
}

/*
 * Name: zmr_zfin
 *
 * Description:
 *   Received ZFIN, respond with ZFIN.  Wait for ZRQINIT or "OO"
 *
 */

static int zmr_zfin(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  /* We are finished and will send ZFIN.  Transition to the ZMR_FINISH state
   * and wait for either ZRQINIT meaning that another file follows or "OO"
   * meaning that we are all done.
   */

  zmdbg("PSTATE %d:%d->%d:%d:  Send ZFIN\n",
         pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);
  zmdbg("ZMR_STATE %d\n", pzm->state);

  pzm->pstate    = ZMR_FINISH;
  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* Release any resource still held from the last file transfer */

  zmr_filecleanup(pzmr);

  /* Let the parser no that "OO" is a possibility */

  pzm->flags    |= ZM_FLAG_OO;

  /* Now send the ZFIN response */

  return zm_sendhexhdr(pzm, ZFIN, g_zeroes);
}

/*
 * Name: zmr_finto
 *
 * Description:
 *   Timedout in state ZMR_FINISH - Sent ZFIN, waiting for "OO"
 *
 */

static int zmr_finto(struct zm_state_s *pzm)
{
  struct zmr_state_s *pzmr = (struct zmr_state_s *)pzm;

  /* Increment the count of timeouts (not really necessary because we are
   * done).
   */

  pzmr->ntimeouts++;
  pzm->flags  &= ~ZM_FLAG_OO; /* No longer expect "OO" */

  zmdbg("ZMR_STATE %d: %d send timeouts\n", pzm->state, pzmr->ntimeouts);

  /* And terminate the reception with a timeout error */

  return -ETIMEDOUT;
}

/*
 * Name: zmr_oo
 *
 * Description:
 *   Received "OO" in the ZMR_FINISH state.  We are finished!
 *
 */

static int zmr_oo(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d: Done\n", pzm->state);
  return ZM_XFRDONE;
}

/*
 * Name: zmr_message
 *
 * Description:
 *   The remote system wants to put a message on stderr
 *
 */

int zmr_message(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d\n", pzm->state);

  /* Setup to receive a data packet.  Enter PSTATE_DATA */

  zm_readstate(pzm);
  return 0;
}

/*
 * Name: zmr_zstderr
 *
 * Description:
 *   The remote system wants to put a message on stderr
 *
 */

static int zmr_zstderr(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d\n", pzm->state);

  pzm->pktbuf[pzm->pktlen] = '\0';
  fprintf(stderr, "Message: %s", (char*)pzm->pktbuf);
  return 0;
}

/*
 * Name: zmr_cmdto
 *
 * Description:
 *   Timed out waiting for command or stderr data
 *
 */

static int zmr_cmdto(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d: Timed out:  No command received\n", pzm->state);

  /* Terminate the reception with a timeout error */

  return -ETIMEDOUT;
}

/*
 * Name: zmr_doneto
 *
 * Description:
 *   Timed out in ZMR_DONE state
 *
 */

static int zmr_doneto(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d: Timeout if ZMR_DONE\n", pzm->state);

  /* Terminate the reception with a timeout error */

  return -ETIMEDOUT;
}

/*
 * Name: zmr_error
 *
 * Description:
 *   An unexpected event occurred in this state
 *
 */

static int zmr_error(struct zm_state_s *pzm)
{
  zmdbg("ZMR_STATE %d: Protocol error, header=%d\n",
        pzm->state, pzm->hdrdata[0]);

  pzm->flags |= ZM_FLAG_WAIT;
  pzm->flags &= ~ZM_FLAG_OO;   /* In case we get here from ZMR_FINISH */
  return 0;
}


/*
 * Name: zmr_openfile
 *
 * Description:
 *   If no output file has been opened to receive the data, then open the
 *   file for output whose name is in pzm->pktbuf.
 *
 */

static int zmr_openfile(struct zmr_state_s *pzmr, uint32_t crc)
{
   uint8_t by[4];
#if 0  
  off_t offset;
 

  /* Has an output file already been opened?  Do we have a file name? */

  if (pzmr->outfd < 0)
    {
      /* No.. We should have a filename from the ZFILE packet? */

      if (!pzmr->filename)
        {
          zmdbg("No filename!\n");
          goto skip;
        }

      /* Yes.. then open this file for output */

      pzmr->outfd = open((char *)pzmr->filename,
                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (pzmr->outfd < 0)
        {
          zmdbg("ERROR: Failed to open %s: %d\n", pzmr->filename, errno);
          goto skip;
        }
    }

  /* Are we appending/resuming a transfer? */

  offset = 0;
  if (pzmr->f0 == ZCRESUM)
    {
      /* Yes... get the current file position */

      offset = lseek(pzmr->outfd, 0, SEEK_CUR);
      if (offset == (off_t)-1)
        {
          zmdbg("ERROR: lseek failed: %d\n", errno);
          goto skip;
        }
    }

  zmdbg("ZMR_STATE %d->%d: Send ZRPOS(%ld)\n",
        pzmr->cmn.state, ZMR_READREADY, (unsigned long)offset);

  pzmr->offset = offset;
#endif 
  
  pzmr->cmn.state = ZMR_READREADY;
  zm_be32toby(pzmr->offset, by);
  return zm_sendhexhdr(&pzmr->cmn, ZRPOS, by);

  /* We get here on any failures.  This file will be skipped. */

// skip:
//   zmdbg("ZMR_STATE %d->%d: Send ZSKIP\n", pzmr->cmn.state, ZMR_START);

//   pzmr->cmn.state = ZMR_START;
//   return zm_sendhexhdr(&pzmr->cmn, ZSKIP, g_zeroes);

}

/*
 * Name: zmr_fileerror
 *
 * Description:
 *   A receiver-detected file error has occurred.  Send Attn followed by
 *   the specified header (ZRPOS or XFERR).
 *
 */

static int zmr_fileerror(struct zmr_state_s *pzmr, uint8_t type,
                         uint32_t data)
{
  uint8_t *src;
  uint8_t *dest;
  uint8_t by[4];

  /* Set the state back to IDLE to abort the transfer */

  zmdbg("PSTATE %d:%d->%d:%d\n",
        pzmr->cmn.pstate, pzmr->cmn.psubstate, PSTATE_IDLE, PIDLE_ZPAD);

  pzmr->cmn.pstate    = PSTATE_IDLE;
  pzmr->cmn.psubstate = PIDLE_ZPAD;

  /* Send Attn */

  if (pzmr->attn != NULL)
    {
      ssize_t nwritten;
      int len;

      /* Copy the attention string to the I/O buffer (pausing is ATTNPSE
       * is encountered.
       */

      dest = pzmr->cmn.pktbuf;
      for (src = (void *)pzmr->attn; *src != '\0'; src++)
        {
          if (*src == ATTNBRK )
            {
#ifdef CONFIG_SYSTEM_ZMODEM_SENDBREAK
              /* Send a break
               * TODO: tcsendbreak() does not yet exist.
               */

              tcsendbreak(pzm->remfd, 0);
#endif
            }
          else if (*src == ATTNPSE)
            {
              /* Pause for 1 second */

            }
          else
            {
              /* Transfer the character */

              *dest++ = *src;
            }
        }

      /* Null-terminate and send */

      *dest++ = '\0';

      len = strlen((char *)pzmr->cmn.pktbuf);
      nwritten = pzmr->cmn.write(&pzmr,pzmr->cmn.pktbuf, len);
      if (nwritten < 0)
        {
          zmdbg("ERROR: write failed: %d\n", (int)nwritten);
          return (int)nwritten;
        }
    }

  /* Send the specified header */

  zm_be32toby(data, by);
  return zm_sendhexhdr(&pzmr->cmn, type, by);
}

/*
 * Name: zmr_filecleanup
 *
 * Description:
 *   Release resources tied up by the last file transfer
 *
 */

static void zmr_filecleanup(struct zmr_state_s *pzmr)
{
  /* Deallocate the file name and attention strings */

  if (pzmr->filename)
    {
      free(pzmr->filename);
      pzmr->filename = NULL;
    }

  if (pzmr->attn)
    {
      free(pzmr->attn);
      pzmr->attn = NULL;
    }
}


/*
 * Name: zm_event
 *
 * Description:
 *   This is the heart of the Zmodem state machine.  Logic initiated by
 *   zm_parse() will detect events and, eventually call this function.
 *   This function will make the state transition, performing any action
 *   associated with the event.
 *
 */

static int zm_event(struct zm_state_s *pzm, int event)
{
  const struct zm_transition_s *ptr;

  zmdbg("ZM[R|S]_state: %d event: %d\n", pzm->state, event);

  /* Look up the entry associated with the event in the current state
   * transition table.  NOTE that each state table must be termined with a
   * ZME_ERROR entry that provides indicates that the event was not
   * expected.  Thus, the following search will always be successful.
   */

  ptr = pzm->evtable[pzm->state];
  while (ptr->type != ZME_ERROR && ptr->type != event)
    {
      /* Skip to the next entry */

      ptr++;
    }

  zmdbg("Transition ZM[R|S]_state %d->%d discard: %d action: %p\n",
        pzm->state, ptr->next, ptr->bdiscard, ptr->action);

  /* Perform the state transition */

  pzm->state = ptr->next;

  /* Discard buffered data if so requrested */

  if (ptr->bdiscard)
    {
      pzm->rcvlen = 0;
      pzm->rcvndx = 0;
    }

  /* And, finally, perform the associated action */

  return ptr->action(pzm);
}

/*
 * Name: zm_nakhdr
 *
 * Description:
 *   Send a NAK in response to a malformed or unsupported header.
 *
 */

static int zm_nakhdr(struct zm_state_s *pzm)
{
  zmdbg("PSTATE %d:%d->%d:%d: NAKing\n",
        pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);

  /* Revert to the IDLE state */

  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* And NAK the header */

  return zm_sendhexhdr(pzm, ZNAK, g_zeroes);
}

/*
 * Name: zm_hdrevent
 *
 * Description:
 *   Process an event associated with a header.
 *
 */

static int zm_hdrevent(struct zm_state_s *pzm)
{
  zmdbg("Received type: %d data: %02x %02x %02x %02x\n",
        pzm->hdrdata[0],
        pzm->hdrdata[1], pzm->hdrdata[2], pzm->hdrdata[3], pzm->hdrdata[4]);
  zmdbg("PSTATE %d:%d->%d:%d\n",
        pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);

  /* Revert to the IDLE state */

  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* Verify the checksum.  16- or 32-bit? */

  if (pzm->hdrfmt == ZBIN32)
    {
      uint32_t crc;

      /* Checksum is over 9 bytes:  The header type, 4 data bytes, plus 4 CRC bytes */

      crc = crc32part(pzm->hdrdata, 9, 0xffffffff);
      if (crc != 0xdebb20e3)
        {
          zmdbg("ERROR: ZBIN32 CRC32 failure: %08x vs debb20e3\n", crc);
          return zm_nakhdr(pzm);
        }
    }
  else
    {
      uint16_t crc;

      /* Checksum is over 7 bytes:  The header type, 4 data bytes, plus 2 CRC bytes */

      crc = crc16part(pzm->hdrdata, 7, 0);
      if (crc != 0)
        {
          zmdbg("ERROR: ZBIN/ZHEX CRC16 failure: %04x vs 0000\n", crc);
          return zm_nakhdr(pzm);
        }
    }

  return zm_event(pzm, pzm->hdrdata[0]);
}

/*
 * Name: zm_dataevent
 *
 * Description:
 *   Process an event associated with a header.
 *
 */

static int zm_dataevent(struct zm_state_s *pzm)
{
  zmdbg("Received type: %d length: %d\n", pzm->pkttype, pzm->pktlen);
  zmdbg("PSTATE %d:%d->%d:%d\n",
        pzm->pstate, pzm->psubstate, PSTATE_IDLE, PIDLE_ZPAD);

  /* Revert to the IDLE state */

  pzm->pstate    = PSTATE_IDLE;
  pzm->psubstate = PIDLE_ZPAD;

  /* Verify the checksum. 16- or 32-bit? */

  if (pzm->hdrfmt == ZBIN32)
    {
      uint32_t crc;

      crc = crc32part(pzm->pktbuf, pzm->pktlen, 0xffffffff);
      if (crc != 0xdebb20e3)
        {
          zmdbg("ERROR: ZBIN32 CRC32 failure: %08x vs debb20e3\n", crc);
          pzm->flags &= ~ZM_FLAG_CRKOK;
        }
      else
        {
          pzm->flags |= ZM_FLAG_CRKOK;
        }

      /* Adjust the back length to exclude the packet type length of the 4-
       * byte checksum.
       */

      pzm->pktlen -= 5;
    }
  else
    {
      uint16_t crc;

      crc = crc16part(pzm->pktbuf, pzm->pktlen, 0);
      if (crc != 0)
        {
          zmdbg("ERROR: ZBIN/ZHEX CRC16 failure: %04x vs 0000\n", crc);
          pzm->flags &= ~ZM_FLAG_CRKOK;
        }
      else
        {
          pzm->flags |= ZM_FLAG_CRKOK;
        }

     /* Adjust the back length to exclude the packet type length of the 2-
      * byte checksum.
      */

      pzm->pktlen -= 3;
    }

  /* Then handle the data received event */

  return zm_event(pzm, ZME_DATARCVD);
}

/*
 * Name: zm_idle
 *
 * Description:
 *   Data has been received in state PSTATE_IDLE.  In this state we are
 *   looking for the beginning of a header indicated by the receipt of
 *   ZDLE.  We skip over ZPAD characters and flush the received buffer in
 *   the case where anything else is received.
 *
 */

static int zm_idle(struct zm_state_s *pzm, uint8_t ch)
{
  switch (ch)
    {
    /* One or more ZPAD characters must precede the ZDLE */

    case ZPAD:
      {
        /* The ZDLE character is expected next */

        zmdbg("PSTATE %d:%d->%d:%d\n",
              pzm->pstate, pzm->psubstate, pzm->pstate, PIDLE_ZDLE);

        pzm->psubstate = PIDLE_ZDLE;
      }
      break;

    /* ZDLE indicates the beginning of a header. */

    case ZDLE:

      /* Was the ZDLE preceded by ZPAD[s]?  If not, revert to the PIDLE_ZPAD
       * substate.
       */

      if (pzm->psubstate == PIDLE_ZDLE)
        {
          zmdbg("PSTATE %d:%d->%d:%d\n",
                pzm->pstate, pzm->psubstate, PSTATE_HEADER, PHEADER_FORMAT);

          pzm->flags    &= ~ZM_FLAG_OO;
          pzm->pstate    = PSTATE_HEADER;
          pzm->psubstate = PHEADER_FORMAT;
          break;
        }
      else
        {
          zmdbg("PSTATE %d:%d->%d:%d\n",
                pzm->pstate, pzm->psubstate, pzm->pstate, PIDLE_ZPAD);

          pzm->psubstate = PIDLE_ZPAD;
        }

    /* O might be the first character of "OO".  "OO" might be part of the file
     * receiver protocol.  After receiving on e file in a group of files, the
     * receiver expected either "OO" indicating that all files have been sent,
     * or a ZRQINIT header indicating the start of the next file.
     */

    case 'O':
      /* Is "OO" a possibility in this context?  Fall through to the default
       * case if not.
       */

      if ((pzm->flags & ZM_FLAG_OO) != 0)
        {
          /* Yes... did we receive an 'O' before this one? */

          if (pzm->psubstate == PIDLE_OO)
            {
              /* This is the second 'O' of "OO". the receiver operation is
               * finished.
               */

              zmdbg("PSTATE %d:%d->%d:%d\n",
                    pzm->pstate, pzm->psubstate, pzm->pstate, PIDLE_ZPAD);

              pzm->flags    &= ~ZM_FLAG_OO;
              pzm->psubstate = PIDLE_ZPAD;
              return zm_event(pzm, ZME_OO);
            }
          else
            {
              /* No... then this is the first 'O' that we have seen */

              zmdbg("PSTATE %d:%d->%d:%d\n",
                    pzm->pstate, pzm->psubstate, pzm->pstate, PIDLE_OO);

              pzm->psubstate = PIDLE_OO;
            }
          break;
        }

    /* Unexpected character.  Wait for the next ZPAD to get us back in sync. */

    default:
      if (pzm->psubstate != PIDLE_ZPAD)
        {
          zmdbg("PSTATE %d:%d->%d:%d\n",
                pzm->pstate, pzm->psubstate, pzm->pstate, PIDLE_ZPAD);

          pzm->psubstate = PIDLE_ZPAD;
        }
      break;
    }

  return 0;
}

/*
 * Name: zm_header
 *
 * Description:
 *   Data has been received in state PSTATE_HEADER (i.e., ZDLE was received
 *   in PSTAT_IDLE).
 *
 *   The following headers are supported:
 *
 *   16-bit Binary:
 *     ZPAD ZDLE ZBIN type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2
 *     Payload length: 7 (type, 4 bytes data, 2 byte CRC)
 *   32-bit Binary:
 *     ZPAD ZDLE ZBIN32 type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2 crc-3 crc-4
 *     Payload length: 9 (type, 4 bytes data, 4 byte CRC)
 *   Hex:
 *     ZPAD ZPAD ZDLE ZHEX type f3/p0 f2/p1 f1/p2 f0/p3 crc-1 crc-2 CR LF [XON]
 *     Payload length: 16 (14 hex digits, cr, lf, ignoring optional XON)
 *
 */

static int zm_header(struct zm_state_s *pzm, uint8_t ch)
{
  /* ZDLE encountered in this state means that the following character is
   * escaped.
   */

  if (ch == ZDLE && (pzm->flags & ZM_FLAG_ESC) == 0)
    {
      /* Indicate that we are beginning the escape sequence and return */

      pzm->flags |= ZM_FLAG_ESC;
      return 0;
    }

  /* Handle the escaped character in an escape sequence */

  if ((pzm->flags & ZM_FLAG_ESC) != 0)
    {
      switch (ch)
        {
        /* Two special cases */

        case ZRUB0:
          ch = ASCII_DEL;
          break;

        case ZRUB1:
          ch = 0xff;
          break;

        /* The typical case:  Toggle bit 6 */

        default:
          ch ^= 0x40;
          break;
        }

      /* We are no longer in an escape sequence */

      pzm->flags &= ~ZM_FLAG_ESC;
    }

  /* Now handle the next character, escaped or not, according to the current
   * PSTATE_HEADER substate.
   */

  switch (pzm->psubstate)
    {
    /* Waiting for the header format {ZBIN, ZBIN32, ZHEX} */

    case PHEADER_FORMAT:
      {
        switch (ch)
          {
          /* Supported header formats */

          case ZHEX:
          case ZBIN:
          case ZBIN32:
            {
              /* Save the header format character. Next we expect the header
               * data payload beginning with the header type.
               */

              pzm->hdrfmt    = ch;
              pzm->psubstate = PHEADER_PAYLOAD;
              pzm->hdrndx    = 0;
            }
            break;

          default:
            {
              /* Unrecognized header format. */

              return zm_nakhdr(pzm);
            }
        }
      }
      break;

    /* Waiting for header payload */

    case PHEADER_PAYLOAD:
      {
        int ndx = pzm->hdrndx;

        switch (pzm->hdrfmt)
          {
          /* Supported header formats */

          case ZHEX:
            {
              if (!isxdigit(ch))
                {
                  return zm_nakhdr(pzm);
                }

              /* Save the MS nibble; setup to receive the LS nibble.  Index
               * is not incremented.
               */

              pzm->hdrdata[ndx] = zm_decnibble(ch) << 4;
              pzm->psubstate    = PHEADER_LSPAYLOAD;
            }
            break;

          case ZBIN:
          case ZBIN32:
            {
              /* Save the payload byte and increment the index. */

              pzm->hdrdata[ndx] = ch;
              ndx++;

              /* Check if the full header payload has bee buffered.
               *
               * The ZBIN format uses 16-bit CRC so the binary length of the
               * full payload is 1+4+2 = 7 bytes; the ZBIN32 uses a 32-bit CRC
               * so the binary length of the payload is 1+4+4 = 9 bytes;
               */

              if (ndx >= 9 || (pzm->hdrfmt == ZBIN && ndx >= 7))
                {
                  return zm_hdrevent(pzm);
                }
              else
                {
                  /* Setup to receive the next byte */

                  pzm->psubstate = PHEADER_PAYLOAD;
                  pzm->hdrndx    = ndx;
                }
            }
            break;

          default: /* Should not happen */
            break;
          }
      }
      break;

    /* Waiting for LS nibble header type (ZHEX only) */

    case PHEADER_LSPAYLOAD:
      {
        int ndx = pzm->hdrndx;

        if (pzm->hdrfmt == ZHEX && isxdigit(ch))
          {
            /* Save the LS nibble and increment the index. */

            pzm->hdrdata[ndx] |= zm_decnibble(ch);
            ndx++;

            /* The ZHEX format uses 16-bit CRC.  So the binary length
             * of the sequence is 1+4+2 = 7 bytes.
             */

            if (ndx >= 7)
              {
                return zm_hdrevent(pzm);
              }
            else
              {
                /* Setup to receive the next MS nibble */

                pzm->psubstate = PHEADER_PAYLOAD;
                pzm->hdrndx    = ndx;
              }
          }
        else
          {
            return zm_nakhdr(pzm);
          }
      }
      break;
    }

  return 0;
}

/*
 * Name: zm_data
 *
 * Description:
 *   Data has been received in state PSTATE_DATA.  PSTATE_DATA is set by
 *   Zmodem transfer logic when it exepects to received data from the
 *   remote peer.
 *
 *   FORMAT:
 *     xx xx xx xx ... xx ZDLE <type> crc-1 crc-2 [crc-3 crc-4]
 *
 *   Where xx is binary data (that may be escaped).  The 16- or 32-bit CRC
 *   is selected based on a preceding header.  ZHEX data packets are not
 *   supported.
 *
 *   When setting pstate to PSTATE_DATA, it is also expected that the
 *   following initialization is performed:
 *
 *   - The crc value is initialized appropriately
 *   - ncrc is set to zero.
 *   - pktlen is set to zero
 *
 */

static int zm_data(struct zm_state_s *pzm, uint8_t ch)
{
  int ret;

  /* ZDLE encountered in this state means that the following character is
   * escaped.  Escaped characters may appear anywhere within the data packet.
   */

  if (ch == ZDLE && (pzm->flags & ZM_FLAG_ESC) == 0)
    {
      /* Indicate that we are beginning the escape sequence and return */

      pzm->flags |= ZM_FLAG_ESC;
      return 0;
    }

  /* Make sure that there is space for another byte in the packet buffer */

  if (pzm->pktlen >= ZM_PKTBUFSIZE)
    {
      zmdbg("ERROR:  The packet buffer is full\n");
      zmdbg("        ch=%c[%02x] pktlen=%d ptktype=%02x ncrc=%d\n",
            isprint(ch) ? ch : '.', ch, pzm->pktlen, pzm->pkttype, pzm->ncrc);
      zmdbg("        rcvlen=%d rcvndx=%d\n",
            pzm->rcvlen, pzm->rcvndx);
      return -1;
    }

  /* Handle the escaped character in an escape sequence */

  if ((pzm->flags & ZM_FLAG_ESC) != 0)
    {
      switch (ch)
        {
        /* The data packet type may immediately follow the ZDLE in PDATA_READ
         * substate.
         */

        case ZCRCW: /* Data packet (Non-streaming, ZACK response expected) */
        case ZCRCE: /* Data packet (End-of-file, no response unless an error occurs) */
        case ZCRCG: /* Data packet (Full streaming, no response) */
        case ZCRCQ: /* Data packet (ZACK response expected) */
          {
            /* Save the packet type, change substates, and set of count that
             * indicates the nubmer of bytes still to be added to the packet
             * buffer:
             *
             *   ZBIN:   1+2 = 3
             *   ZBIN32: 1+4 = 5
             */

            pzm->pkttype   = ch;
            pzm->psubstate = PDATA_CRC;
            pzm->ncrc      = (pzm->hdrfmt == ZBIN32) ? 5 : 3;
          }
          break;

        /* Some special cases */

        case ZRUB0:
          ch = ASCII_DEL;
          break;

        case ZRUB1:
          ch = 0xff;
          break;

        /* The typical case:  Toggle bit 6 */

        default:
          ch ^= 0x40;
          break;
        }

      /* We are no longer in an escape sequence */

      pzm->flags &= ~ZM_FLAG_ESC;
    }

  /* Transfer received data from the I/O buffer to the packet buffer.
   * Accumulate the CRC for the received data.  This includes the data
   * payload plus the packet type code plus the CRC itself.
   */

   pzm->pktbuf[pzm->pktlen++] = ch;
   if (pzm->ncrc == 1)
     {
       /* We are at the end of the packet.  Check the CRC and post the event */

       ret = zm_dataevent(pzm);

       /* The packet data has been processed.  Discard the old buffered
        * packet data.
        */

       pzm->pktlen = 0;
       pzm->ncrc   = 0;
       return ret;
     }
   else if (pzm->ncrc > 1)
     {
       /* We are still parsing the CRC.  Decrement the count of CRC bytes
        * remaining.
        */

       pzm->ncrc--;
     }

  return 0;
}


/*
 * Name: zm_parse
 *
 * Description:
 *   New data from the remote peer is available in pzm->rcvbuf.  The number
 *   number of bytes of new data is given by rcvlen.
 *
 *   This function will parse the data in the buffer and, based on the
 *   current state and the contents of the buffer, will drive the Zmodem
 *   state machine.
 *
 */

static int zm_parse(struct zm_state_s *pzm, size_t rcvlen)
{
  uint8_t ch;
  int ret;
#ifdef __linux__
  assert(pzm && rcvlen <= CONFIG_SYSTEM_ZMODEM_RCVBUFSIZE);
#endif
  zm_dumpbuffer("Received", pzm->rcvbuf, rcvlen);

  /* We keep a copy of the length and buffer index in the state structure.
   * This is only so that deeply nested logic can use these values.
   */

  pzm->rcvlen = rcvlen;
  pzm->rcvndx = 0;

  /* Process each byte until we reach the end of the buffer (or until the
   * data is discarded.
   */

  while (pzm->rcvndx < pzm->rcvlen)
    {
      /* Get the next byte from the buffer */

      ch = pzm->rcvbuf[pzm->rcvndx];
      pzm->rcvndx++;

      /* Handle sequences of CAN characters.  When we encounter 5 in a row,
       * then we consider this a request to cancel the file transfer.
       */

      if (ch == ASCII_CAN)
        {
          if (++pzm->ncan >= 5)
            {
              zmdbg("Remote end has canceled\n");
              pzm->rcvlen = 0;
              pzm->rcvndx = 0;
              return zm_event(pzm, ZME_CANCEL);
            }
        }
      else
        {
          /* Not CAN... reset the sequence count */

          pzm->ncan = 0;
        }

      /* Skip over XON and XOFF */

      if (ch != ASCII_XON && ch != ASCII_XOFF)
        {
          /* And process what follows based on the current parsing state */

          switch (pzm->pstate)
            {
            case PSTATE_IDLE:
              ret = zm_idle(pzm, ch);
              break;

            case PSTATE_HEADER:
              ret = zm_header(pzm, ch);
              break;

            case PSTATE_DATA:
              ret = zm_data(pzm, ch);
              break;

            /* This should not happen */

            default:
              zmdbg("ERROR: Invalid state: %d\n", pzm->pstate);
              ret = -1;
              break;
            }

          /* Handle end-of-transfer and irrecoverable errors by breaking out
           * of the loop and return a non-zero return value to indicate that
           * transfer is complete.
           */

          if (ret != 0)
            {
              zmdbg("%s: %d\n", ret < 0 ? "Aborting" : "Done", ret);
              return ret;
            }
        }
    }

  /* If we made it through the entire buffer with no errors detected, then
   * return OK == 0 meaning that everything is okay, but we are not finished
   * with the transfer.
   */

  return 0;
}
/*
 * Name: zmr_receive
 *
 * Description:
 *   Receive file(s) sent from the remote peer.
 *
 * Input Parameters:
 *   handle - The handler created by zmr_initialize().
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 *
 */

int zmr_receive(struct zmr_state_s *pzmr, int len)
{
  // ssize_t nread;

  /* The first thing that should happen is to receive ZRQINIT from the
   * remote sender.  This could take while so use a long timeout.
   */


  // nread = pzmr->cmn.read(&pzmr,pzmr->cmn.rcvbuf, CONFIG_SYSTEM_ZMODEM_RCVBUFSIZE);
  // if(nread > 0){
  //     return zm_parse(&pzmr->cmn,nread);
  // }
  return zm_parse(&pzmr->cmn,len);
}

/*
 * Name: zmr_initialize
 *
 * Description:
 *   Initialize for Zmodem receive operation
 * 
 * Input Parameters:
 *   write - IO write func.
 *   read - IO read func
 *   on_receive - data receive event func
 * 
 * Returned Value:
 *   An opaque handle that can be use with zmr_receive() and zmr_release().
 *
 */

struct zmr_state_s * zmr_initialize(size_t (*write)(struct zmr_state_s **pzmr, const uint8_t *buffer, size_t buflen), 
                                    size_t (*read)(struct zmr_state_s **pzmr, const uint8_t *buffer, size_t buflen),
                                    size_t (*on_receive)(struct zmr_state_s **pzmr,const uint8_t *buffer, size_t buflen,bool zcnl) ){
  struct zmr_state_s *pzmr;
  /* Allocate a new Zmodem receive state structure */

  pzmr = (struct zmr_state_s*)malloc(sizeof(struct zmr_state_s));
  if (NULL == pzmr){
      zmdbg("ERROR: zmr_state_s failed!\r\n");
    }
  pzmr->cmn.evtable   = g_zmr_evtable;
  pzmr->cmn.state     = ZMR_START;
  pzmr->cmn.pstate    = PSTATE_IDLE;
  pzmr->cmn.psubstate = PIDLE_ZPAD;
  pzmr->cmn.write = write;
  pzmr->cmn.read = read;
  pzmr->cmn.on_receive = on_receive;
  pzmr->cmn.timeout = CONFIG_SYSTEM_ZMODEM_CONNTIME;
  return pzmr;
}
