/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
 */

/* This file contains the socket calls */

#include "defc.h"
#include "scc.h"
#include "glog.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef UNDER_CE        //OG
#include <signal.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#define socklen_t int
#endif

extern Scc scc_stat[2];
extern int g_serial_modem[];


int g_wsastartup_called = 0;
typedef unsigned short USHORT;

/* Usage: scc_socket_init() called to init socket mode */
/*  At all times, we try to have a listen running on the incoming socket */
/* If we want to dial out, we close the incoming socket and create a new */
/*  outgoing socket.  Any hang-up causes the socket to be closed and it will */
/*  then re-open on a subsequent call to scc_socket_open */

void scc_socket_init(int port) {
  Scc     *scc_ptr;

#ifdef _WIN32
  WSADATA wsadata;
  int ret;

  if(g_wsastartup_called == 0) {
    ret = WSAStartup(MAKEWORD(2,0), &wsadata);
    printf("WSAStartup ret: %d\n", ret);
    g_wsastartup_called = 1;
  }
#endif
  scc_ptr = &(scc_stat[port]);
  scc_ptr->state = 1;                   /* successful socket */
  scc_ptr->sockfd = -1;                 /* Indicate no socket open yet */
  scc_ptr->accfd = -1;                  /* Indicate no socket open yet */
  scc_ptr->rdwrfd = -1;                 /* Indicate no socket open yet */
  scc_ptr->socket_state = -2;           /* 0 means talk to "modem" */
                                        /* 1 connected */
  scc_ptr->socket_num_rings = 0;
  scc_ptr->socket_last_ring_dcycs = 0;
  scc_ptr->dcd = 0;                     /* 0 means no carrier */
  scc_ptr->modem_s0_val = 0;
  scc_ptr->host_aux1 = sizeof(struct sockaddr_in);
  scc_ptr->host_handle = malloc(scc_ptr->host_aux1);
  /* Real init will be done when bytes need to be read/write from skt */
}

static int scc_socket_close_handle(SOCKET sockfd) {
  if (sockfd != -1)
  {
#if defined(_WIN32)
    return closesocket(sockfd);             // NW: a Windows socket handle is not a file descriptor
#else
    return close(sockfd);
#endif
  }
  return 0;
}

void scc_socket_maybe_open_incoming(int port, double dcycs) {
  Scc     *scc_ptr;
  struct sockaddr_in sa_in;
  int on;
  int ret;
  SOCKET sockfd;
  int inc;

  inc = 0;

  scc_ptr = &(scc_stat[port]);

  if(scc_ptr->sockfd != -1) {
    /* it's already open, get out */
    return;
  }
  if(scc_ptr->socket_state < 0) {
    /* not initialized; ok, since we need to listen */
    //return;
  }

  printf("scc socket close being called from scc_socket_maybe_open_incoming\n");
  scc_socket_close(port, 0, dcycs);

  scc_ptr->socket_state = 0;
  scc_ptr->socket_num_rings = 0;

  memset(scc_ptr->host_handle, 0, scc_ptr->host_aux1);

  while(1) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    printf("sockfd ret: %d\n", sockfd);
    if(sockfd == -1) {
      perror("socket");
      scc_socket_close(port, 0, dcycs);
      scc_ptr->socket_state = -1;
      return;
    }

    on = 1;
    ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                     (char *)&on, sizeof(on));
    if(ret < 0) {
      perror("setsockopt REUSEADDR");
      scc_socket_close(port, 0, dcycs);
      scc_ptr->socket_state = -1;
      return;
    }

    memset(&sa_in, 0, sizeof(sa_in));
    sa_in.sin_family = AF_INET;
    sa_in.sin_port = htons(6501 + port + inc);
    sa_in.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(sockfd, (struct sockaddr *)&sa_in, sizeof(sa_in));

    if(ret >= 0) {
      ret = listen(sockfd, 1);
      break;
    }
    /* else ret to bind was < 0 */
    perror("bind");
    inc++;
    scc_socket_close_handle(sockfd);
    printf("Trying next port: %d\n", 6501 + port + inc);
    if(inc >= 10) {
      printf("Too many retries, quitting\n");
      scc_socket_close(port, 0, dcycs);
      scc_ptr->socket_state = -1;
      return;
    }
  }

  printf("SCC port %d is at unix port %d\n", port, 6501 + port + inc);

  scc_ptr->sockfd = sockfd;
  scc_ptr->state = 1;                   /* successful socket */

  scc_socket_make_nonblock(port, dcycs);

}

void scc_socket_open_outgoing(int port, double dcycs) {
  Scc     *scc_ptr;
  struct sockaddr_in sa_in;
  struct hostent *hostentptr;
  int on;
  int ret;
  SOCKET sockfd;
  USHORT port_number = 23;

  scc_ptr = &(scc_stat[port]);

  printf("scc socket close being called from scc_socket_open_outgoing\n");
  scc_socket_close(port, 0, dcycs);

  scc_ptr->socket_state = 0;

  memset(scc_ptr->host_handle, 0, scc_ptr->host_aux1);

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  printf("sockfd ret: %d\n", sockfd);
  if(sockfd == -1) {
    perror("socket");
    scc_socket_close(port, 1, dcycs);
    return;
  }
  /* printf("socket ret: %d\n", sockfd); */

  on = 1;
  ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
                   (char *)&on, sizeof(on));
  if(ret < 0) {
    perror("setsockopt REUSEADDR");
    scc_socket_close(port, 1, dcycs);
    return;
  }

  memset(&sa_in, 0, sizeof(sa_in));
  sa_in.sin_family = AF_INET;
  /* ARO: inspect the ATDT command to see if there is a decimal port number declared & if so, use it */
  /* Format: ATDT<host>,<port> */
  /* Example ATDT192.168.1.21,4001 */
  char *comma_ptr = strchr((const char *)&scc_ptr->modem_cmd_str[0], ',');
  if (comma_ptr != NULL) {
    long custom_port = strtol(comma_ptr + 1, NULL, 10);
    *comma_ptr = '\0';             /* null terminate the hostname string at the position of the comma */
    if (custom_port >= 1 && custom_port <= 65535) {
      port_number = (USHORT)custom_port;
    } else {
      printf("Specified port out of range: %ld\n", custom_port);
      scc_socket_close_handle(sockfd);
      scc_socket_close(port, 1, dcycs);
      return;
    }
  }
  sa_in.sin_port = htons(port_number);
  hostentptr = gethostbyname((const char*)&scc_ptr->modem_cmd_str[0]);          // OG Added Cast
  if(hostentptr == 0) {
#if defined(_WIN32)
    fatal_printf("gethostbyname %s failed\n", &scc_ptr->modem_cmd_str[0]);
#else
    fatal_printf("gethostbyname %s failed: %s\n", &scc_ptr->modem_cmd_str[0], hstrerror(h_errno));
#endif
    scc_socket_close_handle(sockfd);
    scc_socket_close(port, 1, dcycs);
    return;
  }
  memcpy(&sa_in.sin_addr.s_addr, hostentptr->h_addr,
         hostentptr->h_length);
  /* The above copies the 32-bit internet address into */
  /*   sin_addr.s_addr.  It's in correct network format */

  ret = connect(sockfd, (struct sockaddr *)&sa_in, sizeof(sa_in));
  if(ret < 0) {
    perror("connect");
    scc_socket_close_handle(sockfd);
    scc_socket_close(port, 1, dcycs);
    return;
  }
  scc_socket_modem_connect(port, dcycs);
  scc_ptr->dcd = 1;                     /* carrier on */
  scc_ptr->socket_state = 1;            /* talk to socket */
  scc_ptr->socket_num_rings = 0;

  printf("SCC port %d is now outgoing to %s on TCP port %d\n", port,
         &scc_ptr->modem_cmd_str[0], port_number);

  scc_ptr->sockfd = sockfd;
  scc_ptr->state = 1;                   /* successful socket */

  scc_socket_make_nonblock(port, dcycs);
  scc_ptr->rdwrfd = scc_ptr->sockfd;
}

void scc_socket_make_nonblock(int port, double dcycs) {
  Scc     *scc_ptr;
  SOCKET sockfd;
  int ret;
#if defined(_WIN32)
  u_long flags;
#else
  int flags;
#endif

  scc_ptr = &(scc_stat[port]);
  sockfd = scc_ptr->sockfd;

#if defined(_WIN32)
  flags = 1;
  ret = ioctlsocket(sockfd, FIONBIO, &flags);
  if(ret != 0) {
    printf("ioctlsocket ret: %d\n", ret);
  }
#else
  flags = fcntl(sockfd, F_GETFL, 0);
  if(flags == -1) {
    perror("fcntl GETFL");
    scc_socket_close(port, 1, dcycs);
    scc_ptr->socket_state = -1;
    return;
  }
  ret = fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
  if(ret == -1) {
    perror("fcntl SETFL");
    scc_socket_close(port, 1, dcycs);
    scc_ptr->socket_state = -1;
    return;
  }
#endif
}

void scc_socket_change_params(int port) {
}

void scc_socket_close(int port, int full_close, double dcycs) {
  Scc     *scc_ptr;
  int rdwrfd;
  SOCKET sockfd;
  int i;

  scc_ptr = &(scc_stat[port]);

  printf("In scc_socket_close, %d, %d, %f\n", port, full_close, dcycs);

  rdwrfd = scc_ptr->rdwrfd;
  if(rdwrfd != -1) {
    printf("socket_close: rdwrfd=%d, closing\n", rdwrfd);
    scc_socket_close_handle(rdwrfd);
  }
  sockfd = scc_ptr->sockfd;
  if(sockfd != -1) {
    printf("socket_close: sockfd=%d, closing\n", sockfd);
    scc_socket_close_handle(sockfd);
  }

  scc_ptr->modem_cmd_len = 0;
  scc_ptr->telnet_mode = 0;
  scc_ptr->telnet_iac = 0;
  for(i = 0; i < 2; i++) {
    scc_ptr->telnet_local_mode[i] = 0;
    scc_ptr->telnet_remote_mode[i] = 0;
    scc_ptr->telnet_reqwill_mode[i] = 0;
    scc_ptr->telnet_reqdo_mode[i] = 0;
  }
  scc_ptr->rdwrfd = -1;
  scc_ptr->sockfd = -1;
  scc_ptr->dcd = 0;
  scc_ptr->socket_num_rings = 0;

  if(!full_close) {
    return;
  }

  if(g_serial_modem[port]) {
    scc_socket_modem_hangup(port, dcycs);
  }

  /* and go back to modem mode */
  scc_ptr->socket_state = 0;
  if(g_serial_modem[port]) {
    scc_ptr->modem_dial_or_acc_mode = 0;
  } else {
    scc_ptr->modem_dial_or_acc_mode = 2;                /* always accept */
  }
}

void scc_accept_socket(int port, double dcycs) {
#ifdef SCC_SOCKETS
  Scc     *scc_ptr;
  int flags;
  int num_rings;
  int rdwrfd;
  int ret;

  scc_ptr = &(scc_stat[port]);

  if(scc_ptr->sockfd == -1) {
    printf("in accept_socket, call socket_open\n");
    scc_socket_maybe_open_incoming(port, dcycs);
  }
  if(scc_ptr->sockfd == -1) {
    return;                     /* just give up */
  }
  if(scc_ptr->rdwrfd == -1) {
    rdwrfd = accept(scc_ptr->sockfd, (struct sockaddr*)scc_ptr->host_handle,
                    (socklen_t*)&(scc_ptr->host_aux1));
    if(rdwrfd < 0) {
      return;
    }

    flags = 0;
    ret = 0;
#if !defined(_WIN32)
    /* For Linux, we need to set O_NONBLOCK on the rdwrfd */
    flags = fcntl(rdwrfd, F_GETFL, 0);
    if(flags == -1) {
      perror("fcntl GETFL");
      return;
    }
    ret = fcntl(rdwrfd, F_SETFL, flags | O_NONBLOCK);
    if(ret == -1) {
      perror("fcntl SETFL");
      return;
    }
#endif
    scc_ptr->rdwrfd = rdwrfd;
    printf("Set port[%d].rdwrfd = %d\n", port, rdwrfd);

    num_rings = 4;
    if(scc_ptr->modem_s0_val > 0) {
      num_rings = scc_ptr->modem_s0_val;
    }
    scc_ptr->socket_num_rings = num_rings;
    scc_ptr->socket_last_ring_dcycs = 0;                /* do ring now*/
    scc_socket_modem_do_ring(port, dcycs);

    /* and send some telnet codes */
    scc_ptr->telnet_reqwill_mode[0] = 0xa;              /* 3=GO_AH and 1=ECHO */
    scc_ptr->telnet_reqdo_mode[0] = 0xa;                /* 3=GO_AH and 1=ECHO */
#if 0
    scc_ptr->telnet_reqdo_mode[1] = 0x4;                /* 34=LINEMODE */
#endif
  }
#endif
}

void scc_socket_telnet_reqs(int port, double dcycs) {
  Scc     *scc_ptr;
  word32 mask, willmask, domask;
  int i, j;

  scc_ptr = &(scc_stat[port]);

  for(i = 0; i < 64; i++) {
    j = i >> 5;
    mask = 1 << (i & 31);
    willmask = scc_ptr->telnet_reqwill_mode[j];
    if(willmask & mask) {
      scc_add_to_writebuf(port, 0xff, dcycs);
      scc_add_to_writebuf(port, 0xfb, dcycs);                   /* WILL */
      scc_add_to_writebuf(port, i, dcycs);
    }
    domask = scc_ptr->telnet_reqdo_mode[j];
    if(domask & mask) {
      scc_add_to_writebuf(port, 0xff, dcycs);
      scc_add_to_writebuf(port, 0xfd, dcycs);                   /* DO */
      scc_add_to_writebuf(port, i, dcycs);
    }
  }
}

void scc_socket_fill_readbuf(int port, int space_left, double dcycs) {
#ifdef SCC_SOCKETS
  byte tmp_buf[256];
  Scc     *scc_ptr;
  int rdwrfd;
  int ret;
  int i;

  scc_ptr = &(scc_stat[port]);

  scc_accept_socket(port, dcycs);
  scc_socket_modem_do_ring(port, dcycs);

  if(scc_ptr->socket_state == 0 && g_serial_modem[port]) {
    /* Just get out, this is modem mode */
    /*  The transmit function stuffs any bytes in receive buf */
    return;
  }
  rdwrfd = scc_ptr->rdwrfd;
  if(rdwrfd < 0) {
    return;                     /* just get out */
  }

  /* Try reading some bytes */
  space_left = MIN(space_left, 256);
  ret = recv(rdwrfd, (char*)tmp_buf, space_left, 0);            // OG Added cast
  if(ret > 0) {
    for(i = 0; i < ret; i++) {
      if(tmp_buf[i] == 0) {
        /* Skip null chars */
        continue;
      }
      scc_socket_recvd_char(port, tmp_buf[i], dcycs);
    }
  } else if(ret == 0) {
    /* assume socket close */
    printf("recv got 0 from rdwrfd=%d, closing\n", rdwrfd);
    scc_socket_close(port, 1, dcycs);
  }
#endif
}

int g_scc_dbg_print_cnt = 50;

void scc_socket_recvd_char(int port, int c, double dcycs) {
  Scc     *scc_ptr;
  word32 locmask, remmask, mask;
  word32 reqwillmask, reqdomask;
  int telnet_mode, telnet_iac;
  int eff_c, cpos;
  int reply;

  scc_ptr = &(scc_stat[port]);

  scc_socket_maybe_open_incoming(port, dcycs);

  telnet_mode = scc_ptr->telnet_mode;
  telnet_iac = scc_ptr->telnet_iac;
  if(c >= 0xf0 || telnet_mode || telnet_iac) {
    g_scc_dbg_print_cnt = 50;
  }
#if 0
  if(g_scc_dbg_print_cnt) {
    printf("Recv: %02x mode: %d\n", c, telnet_mode);
    g_scc_dbg_print_cnt--;
  }
#endif

  eff_c = c;
  if(telnet_iac == 0) {
    if(c == 0xff) {
      scc_ptr->telnet_iac = 0xff;
      return;                                   /* and just get out */
    }
  } else {
    /* last char was 0xff, see if this is 0xff */
    if(c != 0xff) {
      /* this is some kind of command */
      eff_c = eff_c | 0x100;                    /* indicate prev char IAC */
    }
  }
  scc_ptr->telnet_iac = 0;

  mask = 1 << (c & 31);
  cpos = (c >> 5) & 1;
  locmask = scc_ptr->telnet_local_mode[cpos] & mask;
  remmask = scc_ptr->telnet_remote_mode[cpos] & mask;
  reqwillmask = scc_ptr->telnet_reqwill_mode[cpos] & mask;
  reqdomask = scc_ptr->telnet_reqdo_mode[cpos] & mask;
  switch(telnet_mode) {
    case 0:     /* just passing through bytes */
      switch(eff_c) {
        case 0x1fe:             /* DON'T */
        case 0x1fd:             /* DO */
        case 0x1fc:             /* WON'T */
        case 0x1fb:             /* WILL */
        case 0x1fa:             /* SB */
          telnet_mode = c;
          break;
        default:
          if(eff_c < 0x100) {
            scc_add_to_readbuf(port, c, dcycs);
          }
          break;
      }
      break;
    case 3:     /* LINEMODE SB SLC, octet 0 */
      if(eff_c == 0x1f0) {
        /* SE, the end */
        telnet_mode = 0;
      }
      printf("LINEMODE SLC octet 0: %02x\n", c);
      telnet_mode = 4;
      break;
    case 4:     /* LINEMODE SB SLC, octet 1 */
      printf("LINEMODE SLC octet 1: %02x\n", c);
      telnet_mode = 5;
      if(eff_c == 0x1f0) {
        /* SE, the end */
        printf("Got SE at octet 1...\n");
        telnet_mode = 0;
      }
      break;
    case 5:     /* LINEMODE SB SLC, octet 2 */
      printf("LINEMODE SLC octet 2: %02x\n", c);
      telnet_mode = 3;
      if(eff_c == 0x1f0) {
        /* SE, the end */
        printf("Got SE at octet 2...\n");
        telnet_mode = 0;
      }
      break;
    case 34:     /* LINEMODE SB beginning */
      switch(c) {
        case 3:         /* SLC */
          telnet_mode = 3;
          break;
        default:
          telnet_mode = 0xee;                   /* go to SB eat */
          break;
      }
      break;
    case 0xfa:     /* in 0xfa = SB mode, eat up subcommands */
      switch(c) {
        case 34:                /* LINEMODE */
          telnet_mode = 34;
          break;
        default:
          telnet_mode = 0xee;                   /* SB eat mode */
          break;
      }
      break;
    case 0xee:     /* in SB eat mode */
      if(eff_c == 0x1f0) {                      /* SE, end of sub-command */
        telnet_mode = 0;
      } else {
        /* just stay in eat mode */
      }
      break;
    case 0xfe:          /* previous char was "DON'T" */
      if(locmask && (reqwillmask == 0)) {
        /* it's a mode change */
        /* always OK to turn off a mode that we had on */
        scc_add_to_writebuf(port, 0xff, dcycs);
        scc_add_to_writebuf(port, 0xfc, dcycs);                 /* WON'T */
        scc_add_to_writebuf(port, c, dcycs);
      }
      scc_ptr->telnet_local_mode[cpos] &= ~mask;
      scc_ptr->telnet_reqwill_mode[cpos] &= ~mask;
      telnet_mode = 0;
      break;
    case 0xfd:          /* previous char was "DO" */
      reply = 0xfc;
      if(locmask == 0 && (reqwillmask == 0)) {
        /* it's a mode change, send some response  */
        reply = 0xfc;                   /* nack it with WON'T */
        if(c == 0x03 || c == 0x01) {
          reply = 0xfb;                         /* Ack with WILL */
        }
        scc_add_to_writebuf(port, 0xff, dcycs);
        scc_add_to_writebuf(port, reply, dcycs);
        scc_add_to_writebuf(port, c, dcycs);
      }
      if(reqwillmask || (reply == 0xfb)) {
        scc_ptr->telnet_local_mode[cpos] |= mask;
      }
      scc_ptr->telnet_reqwill_mode[cpos] &= ~mask;
      telnet_mode = 0;
      break;
    case 0xfc:          /* previous char was "WON'T" */
      if(remmask && (reqdomask == 0)) {
        /* it's a mode change, ack with DON'T */
        scc_add_to_writebuf(port, 0xff, dcycs);
        scc_add_to_writebuf(port, 0xfe, dcycs);                 /* DON'T */
        scc_add_to_writebuf(port, c, dcycs);
      }
      scc_ptr->telnet_remote_mode[cpos] &= ~mask;
      scc_ptr->telnet_reqdo_mode[cpos] &= ~mask;
      telnet_mode = 0;
      break;
    case 0xfb:          /* previous char was "WILL" */
      reply = 0xfe;             /* nack it with DON'T */
      if(remmask == 0 && (reqdomask == 0)) {
        /* it's a mode change, send some response  */
        if(c == 0x03 || c == 0x01) {
          reply = 0xfd;                         /* Ack with DO */
        }
        scc_add_to_writebuf(port, 0xff, dcycs);
        scc_add_to_writebuf(port, reply, dcycs);
        scc_add_to_writebuf(port, c, dcycs);
      }
      if(reqdomask || (reply == 0xfd)) {
        scc_ptr->telnet_remote_mode[cpos] |= mask;
      }
      scc_ptr->telnet_reqdo_mode[cpos] &= ~mask;
      telnet_mode = 0;
      break;
    default:
      telnet_mode = 0;
      break;
  }
  scc_ptr->telnet_mode = telnet_mode;
}

void scc_socket_empty_writebuf(int port, double dcycs) {
#ifdef SCC_SOCKETS
# if !defined(_WIN32)
  struct sigaction newact, oldact;
# endif
  Scc     *scc_ptr;
  double diff_dcycs;
  int plus_mode;
  int rdptr;
  int wrptr;
  int rdwrfd;
  int done;
  int ret;
  int len;
  int c;
  int i;

  scc_ptr = &(scc_stat[port]);

  /* See if +++ done and we should go to command mode */
  diff_dcycs = dcycs - scc_ptr->out_char_dcycs;
  if((diff_dcycs > 900.0*1000) && (scc_ptr->modem_plus_mode == 3) &&
     (scc_ptr->socket_state >= 1) &&
     (g_serial_modem[port] != 0)) {
    scc_ptr->socket_state = 0;                  /* go modem mode, stay connect*/
    scc_ptr->modem_plus_mode = 0;
    scc_socket_send_modem_code(port, 0, dcycs);
  }

  /* Try writing some bytes */
  done = 0;
  while(!done) {
    rdptr = scc_ptr->out_rdptr;
    wrptr = scc_ptr->out_wrptr;
    if(rdptr == wrptr) {
      done = 1;
      break;
    }
    rdwrfd = scc_ptr->rdwrfd;
    len = wrptr - rdptr;
    if(len < 0) {
      len = SCC_OUTBUF_SIZE - rdptr;
    }
    if(len > 32) {
      len = 32;
    }
    if(len <= 0) {
      done = 1;
      break;
    }

    if(scc_ptr->socket_state < 1 && g_serial_modem[port]) {
      len = 1;
      scc_socket_modem_write(port, scc_ptr->out_buf[rdptr],
                             dcycs);
      ret = 1;
    } else {
      if(rdwrfd == -1) {
        if(g_serial_modem[port]) {
          printf("socket_state: %d, ser_mod: %d, "
                 "rdwrfd: %d\n",
                 scc_ptr->socket_state,
                 g_serial_modem[port], rdwrfd);
        }
        scc_ptr->socket_state = 0;
        scc_socket_maybe_open_incoming(port, dcycs);
        return;
      }
      for(i = 0; i < len; i++) {
        c = scc_ptr->out_buf[rdptr + i];
        plus_mode = scc_ptr->modem_plus_mode;
        diff_dcycs = dcycs - scc_ptr->out_char_dcycs;
        if(c == '+' && plus_mode == 0) {
          if(diff_dcycs > 500*1000) {
            scc_ptr->modem_plus_mode = 1;
          }
        } else if(c == '+') {
          if(diff_dcycs < 800.0*1000) {
            plus_mode++;
            scc_ptr->modem_plus_mode =
              plus_mode;
          }
        } else {
          scc_ptr->modem_plus_mode = 0;
        }
        scc_ptr->out_char_dcycs = dcycs;
      }

#if defined(_WIN32)
      ret = send(rdwrfd, (const char*)&(scc_ptr->out_buf[rdptr]), len, 0);                   // OG Added Cast
# else
      /* ignore SIGPIPE around writes to the socket, so we */
      /*  can catch a closed socket and prepare to accept */
      /*  a new connection.  Otherwise, SIGPIPE kills GSport */
      sigemptyset(&newact.sa_mask);
      newact.sa_handler = SIG_IGN;
      newact.sa_flags = 0;
      sigaction(SIGPIPE, &newact, &oldact);

      ret = send(rdwrfd, &(scc_ptr->out_buf[rdptr]), len, 0);

      sigaction(SIGPIPE, &oldact, 0);
      /* restore previous SIGPIPE behavior */
# endif /* WIN32 */

#if 0
      printf("sock output: %02x\n", scc_ptr->out_buf[rdptr]);
#endif

    }

    if(ret == 0) {
      done = 1;                         /* give up for now */
      break;
    } else if(ret < 0) {
      /* assume socket is dead */
      printf("socket write failed, resuming modem mode\n");
      scc_socket_close(port, 1, dcycs);
      done = 1;
      break;
    } else {
      rdptr = rdptr + ret;
      if(rdptr >= SCC_OUTBUF_SIZE) {
        rdptr = rdptr - SCC_OUTBUF_SIZE;
      }
      scc_ptr->out_rdptr = rdptr;
    }
  }
#endif
}

void scc_socket_modem_write(int port, int c, double dcycs) {
  Scc     *scc_ptr;
  char    *str;
  word32 modem_mode;
  int do_echo;
  int got_at;
  int len;

  scc_ptr = &(scc_stat[port]);

  if(scc_ptr->sockfd == -1) {
    scc_ptr->socket_state = 0;
    scc_socket_maybe_open_incoming(port, dcycs);
  }

  modem_mode = scc_ptr->modem_mode;
  str = (char*)&(scc_ptr->modem_cmd_str[0]);            // OG Added Cast

#if 0
  printf("M: %02x\n", c);
#endif
  do_echo = ((modem_mode & SCCMODEM_NOECHO) == 0);
  len = scc_ptr->modem_cmd_len;
  got_at = 0;
  if(len >= 2 && str[0] == 'a' && str[1] == 't') {
    /* we've got an 'at', do not back up past it */
    got_at = 1;
  }
  if(c == 0x0d) {
    if(do_echo) {
      scc_add_to_readbuf(port, c, dcycs);                       /* echo cr */
      scc_add_to_readbuf(port, 0x0a, dcycs);                    /* echo lf */
    }
    do_echo = 0;                /* already did the echo */
    scc_socket_do_cmd_str(port, dcycs);
    scc_ptr->modem_cmd_len = 0;
    len = 0;
    str[0] = 0;
  } else if(c == 0x08) {
    if(len <= 0) {
      do_echo = 0;                      /* do not go past left margin */
    } else if(len == 2 && got_at) {
      do_echo = 0;                      /* do not erase "AT" */
    } else {
      /* erase a character */
      len--;
      str[len] = 0;
    }
  } else if(c < 0x20) {
    /* ignore all control characters, don't echo */
    /* includes line feeds and nulls */
    do_echo = 0;
  } else {
    /* other characters */
    if(len < SCC_MODEM_MAX_CMD_STR) {
      str[len] = tolower(c);
      str[len+1] = 0;
      len++;
    }
  }
  scc_ptr->modem_cmd_len = len;
  if(do_echo) {
    scc_add_to_readbuf(port, c, dcycs);                 /* echo */
  }
}

void scc_socket_do_cmd_str(int port, double dcycs) {
  Scc     *scc_ptr;
  char    *str;
  int pos, len;
  int ret_val;
  int reg, reg_val;
  int was_amp;
  int c;
  int i;

  scc_ptr = &(scc_stat[port]);

  str = (char*)&(scc_ptr->modem_cmd_str[0]);       // OG Added cast
  printf("Got modem string :%s:=%02x %02x %02x\n", str, str[0], str[1],
         str[2]);

  len = scc_ptr->modem_cmd_len;
  str[len] = 0;
  str[len+1] = 0;
  str[len+2] = 0;
  pos = -1;
  if(len < 2) {
    /* just ignore it */
    return;
  }
  if(str[0] != 'a' || str[1] != 't') {
    return;
  }

  /* Some AT command received--make sure socket 6501/6502 is open */
  printf("Some AT command received, sockfd=%d\n", scc_ptr->sockfd);

  pos = 2 - 1;
  ret_val = 0;                  /* "OK" */
  was_amp = 0;
  while(++pos < len) {
    c = str[pos] + was_amp;
    was_amp = 0;
    switch(c) {
      case '&':                 /* at& */
        was_amp = 0x100;
        break;
      case 'z':                 /* atz */
        scc_ptr->modem_mode = 0;
        scc_ptr->modem_s0_val = 0;
        pos = len;                              /* ignore any other commands */
        break;
      case 'e':                 /* ate = echo */
        c = str[pos+1];
        if(c == '1') {
          scc_ptr->modem_mode &= ~SCCMODEM_NOECHO;
          pos++;
        } else {
          scc_ptr->modem_mode |= SCCMODEM_NOECHO;
          pos++;
        }
        break;
      case 'v':                 /* atv = verbose */
        c = str[pos+1];
        if(c == '1') {
          scc_ptr->modem_mode &= ~SCCMODEM_NOVERBOSE;
          pos++;
        } else {
          scc_ptr->modem_mode |= SCCMODEM_NOVERBOSE;
          pos++;
        }
        break;
      case 'o':                 /* ato = go online */
        printf("ato\n");
        if(scc_ptr->dcd && (scc_ptr->rdwrfd != -1) &&
           (scc_ptr->socket_state == 0)) {
          printf("Going back online\n");
          scc_ptr->socket_state = 1;
          scc_socket_modem_connect(port, dcycs);
          ret_val = -1;
        }
        break;
      case 'h':                 /* ath = hang up */
        printf("ath, hanging up\n");
        scc_socket_close(port, (scc_ptr->rdwrfd != -1), dcycs);
        /* scc_socket_maybe_open_incoming(port, dcycs); */
        /* reopen listen */
        break;
      case 'a':                 /* ata */
        printf("Doing ATA\n");
        scc_socket_do_answer(port, dcycs);
        ret_val = -1;
        break;
      case 'd':                 /* atd */
        pos++;
        c = str[pos];
        if(c == 't' || c == 'p') {
          /* skip tone or pulse */
          pos++;
        }
        /* see if it is 111 */
        if(strcmp(&str[pos], "111") == 0) {
          /* Do PPP! */
        } else {
          /* get string to connect to */
          /* Shift string so hostname moves to str[0] */
          for(i = 0; i < len; i++) {
            str[i] = str[pos];
            if(pos >= len) {
              break;
            }
            pos++;
          }

        }
        scc_ptr->modem_dial_or_acc_mode = 1;
        scc_socket_open_outgoing(port, dcycs);
        ret_val = -1;
        pos = len;                      /* always eat rest of the line */
        break;
      case 's':                 /* atsnn=yy */
        pos++;
        reg = 0;
        while(1) {
          c = str[pos];
          if(c < '0' || c > '9') {
            break;
          }
          reg = (reg * 10) + c - '0';
          pos++;
        }
        if(c == '?') {
          /* display S-register */
          if(reg == 0) {
            scc_add_to_readbufv(port, dcycs,
                                "S0=%d\n",
                                scc_ptr->modem_s0_val);
          }
          break;
        }
        if(c != '=') {
          break;
        }
        pos++;
        reg_val = 0;
        while(1) {
          c = str[pos];
          if(c < '0' || c >'9') {
            break;
          }
          reg_val = (reg_val * 10) + c - '0';
          pos++;
        }
        printf("ats%d = %d\n", reg, reg_val);
        if(reg == 0) {
          scc_ptr->modem_s0_val = reg_val;
        }
        pos--;
        break;
      default:
        /* some command--peek into next chars to finish it */
        while(1) {
          c = str[pos+1];
          if(c >= '0' && c <= '9') {
            /* eat numbers */
            pos++;
            continue;
          }
          if(c == '=') {
            /* eat this as well */
            pos++;
            continue;
          }
          /* else get out */
          break;
        }
    }
  }

  if(ret_val >= 0) {
    scc_socket_send_modem_code(port, ret_val, dcycs);
  }
}

void scc_socket_send_modem_code(int port, int code, double dcycs) {
  Scc     *scc_ptr;
  char    *str;
  word32 modem_mode;

  scc_ptr = &(scc_stat[port]);

  switch(code) {
    case 0: str = "OK"; break;
    case 1: str = "CONNECT"; break;
    case 2: str = "RING"; break;
    case 3: str = "NO CARRIER"; break;
    case 4: str = "ERROR"; break;
    case 5: str = "CONNECT 1200"; break;
    case 13: str = "CONNECT 9600"; break;
    case 16: str = "CONNECT 19200"; break;
    case 25: str = "CONNECT 14400"; break;
    case 85: str = "CONNECT 19200"; break;
    default:
      str = "ERROR";
  }

  printf("Sending modem code %d = %s\n", code, str);

  modem_mode = scc_ptr->modem_mode;
  if(modem_mode & SCCMODEM_NOVERBOSE) {
    /* just the number */
    scc_add_to_readbufv(port, dcycs, "%d", code);
    scc_add_to_readbuf(port, 0x0d, dcycs);
  } else {
    scc_add_to_readbufv(port, dcycs, "%s\n", str);
  }
}

void scc_socket_modem_hangup(int port, double dcycs) {
  scc_socket_send_modem_code(port, 3, dcycs);
}

void scc_socket_modem_connect(int port, double dcycs) {
  /* decide which code to send.  Default to 1 if needed */
  scc_socket_send_modem_code(port, 13, dcycs);          /*13=9600*/
}

void scc_socket_modem_do_ring(int port, double dcycs) {
  Scc     *scc_ptr;
  double diff_dcycs;
  int num_rings;

  scc_ptr = &(scc_stat[port]);
  num_rings = scc_ptr->socket_num_rings;
  if(num_rings > 0 && scc_ptr->socket_state == 0) {
    num_rings--;
    diff_dcycs = dcycs - scc_ptr->socket_last_ring_dcycs;
    if(diff_dcycs < 2.0*1000*1000 && g_serial_modem[port]) {
      return;                           /* nothing more to do */
    }
    printf("In modem_do_ring, ringing at %f\n", dcycs);
    if(g_serial_modem[port]) {
      scc_socket_send_modem_code(port, 2, dcycs);                   /* RING */
    } else {
      num_rings = 0;
    }
    scc_ptr->socket_num_rings = num_rings;
    scc_ptr->socket_last_ring_dcycs = (int)dcycs;
    if(num_rings <= 0) {
      /* decide on answering */
      if(scc_ptr->modem_s0_val || (g_serial_modem[port]==0)) {
        scc_socket_do_answer(port, dcycs);
      } else {
        printf("No answer, closing socket\n");
        scc_socket_close(port, 0, dcycs);
      }
    }
  }
}

void scc_socket_do_answer(int port, double dcycs) {
  Scc     *scc_ptr;

  scc_ptr = &(scc_stat[port]);
  scc_ptr->modem_dial_or_acc_mode = 2;
  scc_accept_socket(port, dcycs);
  if(scc_ptr->rdwrfd == -1) {
    printf("Answer when rdwrfd=-1, closing\n");
    scc_socket_close(port, 1, dcycs);
    /* send NO CARRIER message */
  } else {
    scc_ptr->socket_state = 1;
    scc_socket_telnet_reqs(port, dcycs);
    printf("Send telnet reqs, rdwrfd=%d\n", scc_ptr->rdwrfd);
    if(g_serial_modem[port]) {
      scc_socket_modem_connect(port, dcycs);
    }
    scc_ptr->dcd = 1;                           /* carrier on */
    scc_ptr->socket_state = 1;             /* talk to socket */
    scc_ptr->socket_num_rings = 0;
  }
}
