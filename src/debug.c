/*
   GSPLUS - Advanced Apple IIGS Emulator Environment
   Based on the KEGS emulator written by Kent Dickey
   See COPYRIGHT.txt for Copyright information
   See LICENSE.txt for license (GPL v2)
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include "defc.h"
#include "debug.h"
#include "glog.h"

// DISASSEMBLER STUFF
#include "disasm.h"

// STEPPING/ENGINE STUFF
extern Engine_reg engine;
extern int g_config_control_panel;
int g_dbg_enable_port = 0;
int debug_pause = 0;
int g_dbg_step = 0;
int step_count = 0;

extern int g_stepping;

// emulator command stuff
extern int g_limit_speed;
extern int g_screenshot_requested;

// all the message types
typedef enum {
  G_DBG_COMMAND_TEST = 0,
  G_DBG_COMMAND_HELLO = 1,
  G_DBG_COMMAND_STEP = 2,
  G_DBG_COMMAND_CONTINUE = 3,
  G_DBG_COMMAND_GET_CPU = 4,
  G_DBG_COMMAND_SET_CPU = 5,
  G_DBG_COMMAND_GET_MEM = 6,
  G_DBG_COMMAND_SET_MEM = 7,
  G_DBG_COMMAND_ADD_BRK = 8,
  G_DBG_COMMAND_DEL_BRK = 9,
  G_DBG_COMMAND_GET_BRK = 0xA,
  G_DBG_COMMAND_PAUSE = 0xB,                                    // deprecated
  G_DBG_COMMAND_DEBUGGER = 0xC,                         // deprecated
  G_DBG_COMMAND_QUIT = 0xD,                                             // deprecated
  G_DBG_COMMAND_EMU_CMD = 0xE
} G_DBG_COMMANDS;


// incoming commands - 1 char command + 256 char data
#define DBG_CMD_QUEUE_MAXLEN 50
int dbg_cmd_queue_len = 0;
struct dbg_cmd
{
  unsigned int command;
  char cdata[256];
};
struct dbg_cmd dbg_cmd_queue[DBG_CMD_QUEUE_MAXLEN];



// outgoing response messages - can be batched up
// this one can be bigger because it just has pointers to messages
// and we send multiple messages for debug (a "chain")
#define DBG_MSG_QUEUE_MAXLEN 30
int dbg_msg_queue_len = 0;
struct dbg_msg
{
  unsigned int msg_size;
  char *msg_str;
};
struct dbg_msg dbg_msg_queue[DBG_MSG_QUEUE_MAXLEN];

extern int get_byte_at_address(int addr);
extern void set_byte_at_address(int addr, int value);

//b64 funcs
int b64encode_len(int);
int b64encode(char *encoded, const char *string, int len);


void debug_wait_hello();
void debug_setup_socket();
void debug_pop_message();
void debug_push_message(G_DBG_COMMANDS msg_type, char *msg_ptr, int msg_len);


void event_hello();
void event_test_command(char *str);
void event_pause();
void event_cpu_info();
void event_set_cpu();
void event_get_mem(char *str);
void event_did_step(int step_count);
void event_add_brk(char *str);
void event_del_brk(char *str);
void event_get_brk();
void event_emu_cmd(char *str);
void event_set_mem(char *str);


void api_write_socket();
void write_array_start();
void write_array_end();
void write_array_next();
int writeStrToClient(int sckt, const char *str);



int do_dis_json(char *buf, word32 kpc, int accsize, int xsize, int op_provided, word32 instr, int chain);


// derived from?
///  https://www.ibm.com/support/knowledgecenter/ssw_i5_54/rzab6/poll.htm
#define TRUE             1
#define FALSE            0

int len, rc, on = 1;
int listen_sd = -1, new_sd = -1;
int desc_ready, end_server = FALSE, compress_array = FALSE;
int close_conn;
#define BUFFER_SIZE 128
char buffer[BUFFER_SIZE];
char tmp_buffer_4k[4097]; // adding +1 in case someone forgets \0
char tmp_buffer2_4k[4097]; // adding +1 in case someone forgets \0

struct sockaddr_in addr;
int timeout;  // poll timeout in ms
struct pollfd fds[200];
int nfds = 0, current_size = 0, i, j;
int debugger_sd = -1; // this holds our socket file descriptor for the debugger, once attached



/* socket debug version */
void do_go_debug()      {
  while (1) {
    if (g_dbg_step >= 0) {
      if (g_dbg_step == 1) {
        g_dbg_step = -1;        // we are taking a step, so chill on the next round
      }
      g_config_control_panel = 0;
      clear_halt();


      glog("calling run_prog()");
      run_prog();               // also calls debug_server_poll()
      glog("left run_prog()");
      step_count++;

      // so this is a break...
      // we need a pool waiter for a continue or go here

      event_did_step(step_count);
    }

    timeout = 1000;
    debug_server_poll();
    printf(".");
    if (debug_events_waiting() > 0) {
      timeout = 0;
      debug_handle_event();
    }
    g_config_control_panel = 1;
  }
}

// removes the message from the front of the queue and slides the rest over
void debug_pop_message() {
  if (dbg_cmd_queue_len > 0) {
    for (int i = 0; i< dbg_cmd_queue_len; i++) {
      dbg_cmd_queue[i]=dbg_cmd_queue[i+1];  // does this copy bad stuff if we hit max?
    }
    dbg_cmd_queue_len--;
  }
}

// adds message to end of queue and ups the counter
void debug_push_message(G_DBG_COMMANDS msg_type, char *msg_ptr, int msg_len) {
  glogf("debug_push_message() GOT: %d", (int) msg_type);
  dbg_cmd_queue[dbg_cmd_queue_len].command = msg_type;
  memcpy(dbg_cmd_queue[dbg_cmd_queue_len].cdata, msg_ptr, msg_len);  // stripping that first char
  dbg_cmd_queue[dbg_cmd_queue_len].cdata[msg_len+1] = '\0';     // terminator
  dbg_cmd_queue_len++;  // INC POINTER
}

// this should be sufficient
int debug_events_waiting() {
  return dbg_cmd_queue_len;
}

// handle event onfront of queue and remove it
void debug_handle_event() {
  while (debug_events_waiting() > 0) {
    switch (dbg_cmd_queue[0].command) {
      case G_DBG_COMMAND_TEST:   //0
        event_test_command(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_HELLO:   //1
        event_hello();
        break;
      case G_DBG_COMMAND_PAUSE:   //2
        event_pause();
        break;
      case G_DBG_COMMAND_STEP:    //3
        if (g_dbg_step == -1) {
          g_dbg_step = 1; // take a step
        } else {
          g_dbg_step = -1; // first one just halts
        }
        g_stepping = 1;
        break;
      case G_DBG_COMMAND_CONTINUE:  //4
        g_dbg_step = 0;
        g_stepping = 0;
        step_count = 0;
        break;
      case G_DBG_COMMAND_GET_MEM:  //6
        event_get_mem(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_SET_MEM:  //7
        event_set_mem(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_QUIT:
        exit(0);  // HALT!
        break;
      case G_DBG_COMMAND_DEBUGGER:
        //do_debug_intfc();
        break;
      case G_DBG_COMMAND_SET_CPU:
        event_set_cpu(&dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_GET_CPU:
        event_cpu_info();
        break;
      case G_DBG_COMMAND_ADD_BRK:
        event_add_brk(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_DEL_BRK:
        event_del_brk(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_GET_BRK:
        event_get_brk(dbg_cmd_queue[0].cdata);
        break;
      case G_DBG_COMMAND_EMU_CMD:
        event_emu_cmd(dbg_cmd_queue[0].cdata);
        break;
      default:
        break;
    }
    debug_pop_message();
  }
}

void push_api_msg(int size, char *msg_str) {
  if (dbg_msg_queue_len < DBG_MSG_QUEUE_MAXLEN) {
    dbg_msg_queue[dbg_msg_queue_len].msg_size = size;
    dbg_msg_queue[dbg_msg_queue_len].msg_str = msg_str;
    //printf("Latest message : \n%s\n", dbg_msg_queue[dbg_msg_queue_len].msg_str);
    dbg_msg_queue_len++;  // INC POINTER
  } else {
    glog("ABORT! Message dropped because dbg_msg_queue is full.");
  }
}

void api_push_memack() {
  int size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),"{\"type\":\"memack\"}");
  char *msg = (char*)malloc(sizeof(char) * size + 1);
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(size, msg);
}

void api_push_cpu() {
  Engine_reg *eptr;
  eptr = &engine;
  int tmp_acc, tmp_x, tmp_y, tmp_psw;
  int kpc, direct_page, dbank, stack;

  kpc = eptr->kpc;
  tmp_acc = eptr->acc;
  direct_page = eptr->direct;
  dbank = eptr->dbank;
  stack = eptr->stack;
  tmp_x = eptr->xreg;
  tmp_y = eptr->yreg;
  tmp_psw = eptr->psr;

  int size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),"{\"type\":\"cpu\",\"data\":{\"K\":\"%02X\",\"PC\":\"%04X\",\"A\":\"%04X\"," \
                      "\"X\":\"%04X\",\"Y\":\"%04X\",\"S\":\"%04X\",\"D\":\"%04X\",\"B\":\"%02X\",\"PSR\":\"%04X\"}}",
                      kpc>>16, kpc & 0xffff,tmp_acc,tmp_x,tmp_y,stack,direct_page,dbank, tmp_psw & 0xFFF);

  char *msg = (char*)malloc(sizeof(char) * size);
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(size, msg);
}

void api_push_brk() {
  int i;
  extern int g_num_bp_breakpoints;
  extern word32 g_bp_breakpoints[];

  // build our json array of breakpoints
  tmp_buffer2_4k[0] = '\0';   // start with empty string
  char *str_ptr = tmp_buffer2_4k;
  for(i = 0; i < g_num_bp_breakpoints; i++) {
    //printf("{\"bp:%02x: %06x\n", i, g_breakpts[i]);
    str_ptr += sprintf(str_ptr, "{\"trig\":\"addr\",\"match\":\"%06X\"}", g_bp_breakpoints[i]);
    if (i < g_num_bp_breakpoints-1) {
      str_ptr += sprintf(str_ptr, ",");
    }
  }
  str_ptr = tmp_buffer2_4k;     // reset pointer to beginning of string

  int size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),"{\"type\":\"brk\",\"data\":{\"breakpoints\":[%s]}}", str_ptr);

  char *msg = (char*)malloc(sizeof(char) * size);
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(size, msg);
}


void api_push_stack() {
  Engine_reg *eptr;
  eptr = &engine;
  int kpc, stack;

  kpc = eptr->kpc;
  stack = eptr->stack;
  word32 stack_loc = (kpc & 0xff0000) + (stack & 0xff00);       // e.g. 0x00000100  (default)   or 0x00FF0100  (GS Boot)

  // build our json array of 256 stack values ($nn00-nnFF)
  char *str_ptr = tmp_buffer2_4k;       // (1024B)
  for (int i = 0; i<256; i++ ) {
    unsigned int instruction = (int)get_memory_c(stack_loc+i, 0) & 0xff;
    str_ptr += sprintf(str_ptr, "\"%02X\"", instruction);
    if (i < 255) {
      str_ptr += sprintf(str_ptr, ",");
    }
  }
  str_ptr = tmp_buffer2_4k;     // reset pointer to beginning of string

  int size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),"{\"type\":\"stack\",\"data\":{\"loc\":\"%06X\",\"S\":\"%04X\",\"bytes\":[%s]}}",
                      stack_loc,stack,str_ptr);

  char *msg = (char*)malloc(sizeof(char) * size + 1);
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(size, msg);
}


void api_push_disassembly_start() {
  int size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),"{\"type\":\"dis0\"}");
  char *msg = (char*)malloc(sizeof(char) * size + 1);
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(size, msg);
}


void api_push_disassembly() {
  int byte_size;
  int size_mem_imm, size_x_imm;


  // check accumulator size
  size_mem_imm = 2;
  if(engine.psr & 0x20) {
    size_mem_imm = 1;
  }
  // check xy size
  size_x_imm = 2;
  if(engine.psr & 0x10) {
    size_x_imm = 1;
  }

  //disassemble chain from now -> to -> future
  int disassemble_next = 10;  // how many instructions
  int kpc = engine.kpc;       // get starting address
  for (int i = 0; i<disassemble_next; i++) {
    byte_size = do_dis_json(tmp_buffer_4k, kpc, size_mem_imm, size_x_imm, 0, 0, i);
    char *msg = (char*)malloc(sizeof(char) * (strlen(tmp_buffer_4k)+1));
    strcpy(msg,tmp_buffer_4k);
    push_api_msg(strlen(tmp_buffer_4k)+1, msg);
    kpc = kpc + byte_size;
  }
}

void api_push_disassembly_chain() {
  int byte_size;
  int size_mem_imm, size_x_imm;

  // check accumulator size
  size_mem_imm = 2;
  if(engine.psr & 0x20) {
    size_mem_imm = 1;
  }
  // check xy size
  size_x_imm = 2;
  if(engine.psr & 0x10) {
    size_x_imm = 1;
  }
  // then disassemble
  byte_size = do_dis_json(tmp_buffer_4k, engine.kpc, size_mem_imm, size_x_imm, 0, 0, 0);
  char *msg = (char*)malloc(sizeof(char) * (strlen(tmp_buffer_4k)+1));
  strcpy(msg,tmp_buffer_4k);
  push_api_msg(strlen(tmp_buffer_4k)+1, msg);
}


void api_push_dump(int bank, int start, int end) {
  char *post_str = "\"}}";
  int pre_size = snprintf(tmp_buffer_4k, sizeof(tmp_buffer_4k),
                          "{\"type\":\"mem\",\"data\":{\"bank\":\"%02X\",\"start\":\"%04X\",\"end\":\"%04X\",\"b64data\":\"",
                          bank, start, end);

  int len = end - start + 1; // +1 for inclusive
  if (len <= 0x10000) {
    int b64len = b64encode_len(len);
    // get pointer to length of full message with data and json wrapper
    char *msg_json = (char*)malloc(sizeof(char) * (b64len + strlen(post_str) + pre_size + 1));  //? +1
    char *str_ptr = msg_json;                                           // msg_json is our big malloced buffer

    strcat(str_ptr, tmp_buffer_4k);     // prefix string from above
    str_ptr += strlen(tmp_buffer_4k);   // move forward the ptr
    // space to copy - can't figure out if i need to go through get_memory_c() or not
    char *memchunk = (char*)malloc(sizeof(char) * (len+1));
    for (int i = start; i <= end; i++) {
      int kpc = (bank << 16) | i;
      //memchunk[i] = get_memory_c(kpc,0) & 0xFF;		// not efficient but oh well
      memchunk[i] = get_byte_at_address(kpc);
    }

    b64encode(str_ptr, memchunk, len);

    str_ptr = strcat(str_ptr, post_str);    // "\"}}"
    push_api_msg(strlen(msg_json), msg_json);           // don't free, de-queue will do that after writing to socket
    //free(msg_json);
    free(memchunk);
  }
}

void event_cpu_info() {
  api_push_cpu();
  api_write_socket();
}

void event_pause() {
  if (debug_pause) {
    // UNPAUSE
    debug_pause = 0;
  } else {
    // PAUSE IT
    timeout = 200;
    debug_pause = 1;
    while (debug_pause) {
      glog("PAUSED");
      debug_handle_event();
      debug_server_poll();
    }
    timeout = 0;
  }
}


// test command stubs for more interactive testing
void event_test_command(char *str) {
  int addr = 0;
  sscanf(str, "%06X", &addr);
  int foo = get_byte_at_address(addr);
  foo ^= 0xff;
  set_byte_at_address(addr,foo);
}

void event_test_command_bankptr(char *str) {
  int bank = 0;
  sscanf(str, "%02X", &bank);
  show_bankptrs(bank);
}

void event_hello() {
  api_push_stack();
  api_push_cpu();
  api_push_disassembly();
  api_push_disassembly_start();
  // push default startup info on "hello"
  api_write_socket();
}

void event_did_step(int step_count) {
  api_push_stack();
  api_push_cpu();
  api_push_disassembly();
  if (step_count == 1 || g_dbg_step == -2) {    // our first step?
    api_push_disassembly_start();
  }
  api_write_socket();
}


void event_get_mem(char *str) {
  unsigned int bank = 0;
  unsigned int start = 0;
  unsigned int end = 0;
  // get parameters, this format:
  // "6|00:2000-2100"

  int n = sscanf(str, "|%02x:%04x-%04x\n", &bank, &start, &end );
  // found all inputs?
  if (n == 3) {
    api_push_dump(bank, start, end);
    api_write_socket();
  }

}


void handle_set_bytes(int address, char *bytes_data) {
  int byte;
  while (sscanf(bytes_data, "%02x", &byte) == 1) {
    printf("$%02x <---- BYTE @ $%06X\n", byte, address );
    bytes_data += 2;
    set_byte_at_address(address, byte & 0xFF);
    address++;
  }
}


void event_set_mem(char *str) {
  int address = 0;
  char *bytes_data = NULL;

  char *pch;
  pch = strtok (str," ");   // split our memory sets on spaces
  while (pch != NULL) {
    sscanf(pch, "%06X", &address);

    bytes_data = pch+6;
    printf("BytesData %s\n", bytes_data);

    // for each token go try to handle it
    handle_set_bytes(address, bytes_data);

    pch = strtok (NULL, " ");
  }
  api_push_memack();  // send ack
  api_write_socket();
  return;
}


void handle_emu_cmd(char cmd_char, char *cmd_data) {
  int int_value = 0;  // reuseable variable

  switch (cmd_char) {
    // f = fullscreen
    case 'f':
      sscanf(cmd_data, "%d", &int_value);
      x_full_screen(int_value);
      break;
    // s = speed select
    case 's':
      sscanf(cmd_data, "%d", &int_value);
      if (int_value >= 0  && int_value <= 3) {
        printf("Changing speed from %d  to -> %d\n", g_limit_speed, int_value);
        g_limit_speed = int_value;
      } else {
        printf("Specified speed out of range (%d)\n", int_value);
      }
      break;
    case 'v':
      g_screenshot_requested = 1;
      break;
  }
}

void event_emu_cmd(char *str) {
  // split our commands on spaces
  char cmd_char = '\0';
  char *cmd_data = NULL;

  char * pch;
  pch = strtok (str," ");
  while (pch != NULL)
  {
    cmd_char = pch[0];
    cmd_data = pch+1;
    if (cmd_data[0] == '\0') {
      cmd_data = NULL;
    }
    // for each token go try to handle it
    handle_emu_cmd(cmd_char, cmd_data);

    pch = strtok (NULL, " ");
  }
  return;
}


void event_add_brk(char *str) {
  int addr = 0;
  sscanf(str, "%06X", &addr);
  addr = addr & 0xFFFFFF;  // 24 bit KPC address for 65816
  set_bp('B', addr);
  api_push_brk();
  api_write_socket();

  return;
}


void event_del_brk(char *str) {
  int addr = 0;
  sscanf(str, "%06X", &addr);
  addr = addr & 0xFFFFFF;  // 24 bit KPC address for 65816
  delete_bp('B', addr);
  api_push_brk();
  api_write_socket();

  return;
}


void event_get_brk(char *str) {
  api_push_brk();
  api_write_socket();

  return;
}


void handle_cpu_cmd(char cmd_char, char *cmd_data) {
  Engine_reg *eptr;
  eptr = &engine;
  int new_acc, new_x, new_y;
  int new_kpc, new_d, new_b, new_s, new_psr;

  switch (cmd_char) {
    case 'k':
      sscanf(cmd_data, "%06x", &new_kpc);
      new_kpc &= 0xFFFFFF;  // 24-bit clamp
      eptr->kpc = new_kpc;
      break;
    case 'a':
      sscanf(cmd_data, "%04x", &new_acc);
      new_acc &= 0xFFFF;  // 24-bit clamp
      eptr->acc = new_acc;
      break;
    case 'x':
      sscanf(cmd_data, "%04x", &new_x);
      new_x &= 0xFFFF;  // 24-bit clamp
      eptr->xreg = new_x;
      break;
    case 'y':
      sscanf(cmd_data, "%04x", &new_y);
      new_y &= 0xFFFF;  // 24-bit clamp
      eptr->yreg = new_y;
      break;
    case 's':
      sscanf(cmd_data, "%04x", &new_s);
      new_s &= 0xFFFF;  // 24-bit clamp
      eptr->stack = new_s;
      break;
    case 'd':
      sscanf(cmd_data, "%04x", &new_d);
      new_d &= 0xFFFF;  // 24-bit clamp
      eptr->direct = new_d;
      break;
    case 'b':
      sscanf(cmd_data, "%02x", &new_b);
      new_b &= 0xFF;  // 24-bit clamp
      eptr->dbank = new_b;
      break;
    case 'p':
      sscanf(cmd_data, "%04x", &new_psr);
      new_psr &= 0xFFFF;  // 24-bit clamp
      eptr->psr = new_psr;
      break;
    default:
      printf("UNKNOWN CPU COMMAND\n");
      break;
  }
}


void event_set_cpu(char *str) {
  // split our commands on spaces
  char cmd_char = '\0';
  char *cmd_data = NULL;

  char * pch;
  pch = strtok (str," ");
  while (pch != NULL) {
    cmd_char = pch[0];
    cmd_data = pch+1;
    if (cmd_data[0] == '\0') {
      cmd_data = NULL;
    }
    // for each token go try to handle it
    handle_cpu_cmd(cmd_char, cmd_data);

    pch = strtok (NULL, " ");
  }

  api_push_stack();
  api_push_cpu();
  api_push_disassembly();

  api_write_socket();
}


void debug_init() {
  if (g_dbg_enable_port > 0) {
    // g_dbg_enable_port should be enabled by
    glogf("Debug port enabled on: %d", g_dbg_enable_port);
    debug_setup_socket();
    debug_server_poll();
    debug_wait_hello();
    // message is not popped off here because default behavior to send our cpu hello info to client
  } else {
    end_server = TRUE;
    glog("Debug port not enabled");
  }
}


void debug_setup_socket() {
  /*************************************************************/
  /* Create an AF_INET stream socket to receive incoming       */
  /* connections on                                            */
  /*************************************************************/
  listen_sd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sd < 0) {
    perror("socket() failed");
    exit(-1);
  }


  /*************************************************************/
  /* Allow socket descriptor to be reuseable                   */
  /*************************************************************/
  rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
  if (rc < 0) {
    perror("setsockopt() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Set socket to be nonblocking. All of the sockets for    */
  /* the incoming connections will also be nonblocking since  */
  /* they will inherit that state from the listening socket.   */
  /*************************************************************/
  //rc = ioctl(listen_sd, FIONBIO, (char *)&on);
  int flags = fcntl(listen_sd, F_GETFL, 0);
  rc = fcntl(listen_sd, F_SETFL, flags | O_NONBLOCK);
  if (rc < 0) {
    perror("ioctl()/fcntl() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Bind the socket                                           */
  /*************************************************************/
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(g_dbg_enable_port);
  rc = bind(listen_sd, (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0) {
    perror("bind() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Set the listen back log                                   */
  /*************************************************************/
  rc = listen(listen_sd, 32);
  if (rc < 0) {
    perror("listen() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Initialize the pollfd structure                           */
  /*************************************************************/
  memset(fds, 0, sizeof(fds));

  /*************************************************************/
  /* Set up the initial listening socket                        */
  /*************************************************************/
  fds[0].fd = listen_sd;
  fds[0].events = POLLIN;
  nfds = 1;
  /*************************************************************/
  /* Initialize the timeout to 3 minutes. If no               */
  /* activity after 3 minutes this program will end.           */
  /* timeout value is based on milliseconds.                   */
  /*************************************************************/
  timeout = (3 * 60 * 1000);
}


// builds our big json array of commands eg "[{},{},{},...]"
void api_write_socket() {
  const char *comma = ",";
  const char *lbrack = "[";
  const char *rbrack = "]\r\n\r\n";
  unsigned int message_size = 0;
  char *message_string = NULL;
  for (int i = 0; i < dbg_msg_queue_len; i++) {
    message_size = message_size + dbg_msg_queue[i].msg_size;  // for sub-string sizes
  }
  message_size += dbg_msg_queue_len;    // for ","
  message_size += strlen(lbrack) + strlen(rbrack);  // for "[]" + "\0"
  message_string = malloc(sizeof(char)*message_size);
  message_string[0] = '\0'; // necessary?

  char *str_ptr = message_string;       // message_string is alloc'd above to message_size
  str_ptr = strcat(str_ptr, lbrack);    // "["
  for (int i = 0; i < dbg_msg_queue_len; i++) {
    str_ptr = strcat(str_ptr, dbg_msg_queue[i].msg_str);
    if (dbg_msg_queue_len > 1 && i < dbg_msg_queue_len-1) {        // only split with comma if more than 1 item and not last item
      str_ptr = strcat(str_ptr, comma); // ","
    }
    free(dbg_msg_queue[i].msg_str);     // here we go!  oh boy!
  }
  str_ptr = strcat(str_ptr, rbrack);    // "]"
  // message_string is now built!  we can send it.
  dbg_msg_queue_len = 0;                // clear msg queue

  writeStrToClient(debugger_sd, message_string);                // ignores result
  free(message_string); // assuming it was all written :P
}


void write_array_start() {
  const char *brack = "[";
  write(debugger_sd, brack, strlen(brack));
}


void write_array_end() {
  const char *brack = "]\r\n\r\n";
  write(debugger_sd, brack, strlen(brack));
}


void write_array_next() {
  const char *com = ", "; // i'm so neat
  write(debugger_sd, com, strlen(com));
}


//    // write(debugger_sd, buffer, strlen(buffer));
//also for base 64  http://stackoverflow.com/questions/342409/how-do-i-base64-encode-decode-in-c
//http://stackoverflow.com/questions/32749925/sending-a-file-over-a-tcp-ip-socket-web-server
int writeDataToClient(int sckt, const void *data, int datalen) {
  const char *pdata = (const char*) data;

  while (datalen > 0) {
    int numSent = send(sckt, pdata, datalen, 0);
    if (numSent <= 0) {
      if (numSent == 0) {
        printf("The client was not written to: disconnected\n");
      } else {
        perror("The client was not written to");
      }
      return FALSE;
    }
    pdata += numSent;
    datalen -= numSent;
  }

  return TRUE;
}


int writeStrToClient(int sckt, const char *str) {
  return writeDataToClient(sckt, str, strlen(str));
}


// @todo: probably clean up- this was a hack to allow preloading commands
void debug_wait_hello() {
  int hello_received = FALSE;
  timeout = 1000; // 1 sec
  while (hello_received == FALSE) {
    debug_server_poll();
    if (debug_events_waiting() > 0) {
      if (dbg_cmd_queue[0].command == G_DBG_COMMAND_HELLO) {
        hello_received = TRUE;
      } else {
        debug_handle_event();
      }
    }

  }
  timeout = 0; // instantaneous
}


void debug_server_poll() {
  if (end_server == FALSE) {
    /***********************************************************/
    /* Call poll() and wait for it to complete/timeout.        */
    /***********************************************************/
    rc = poll(fds, nfds, timeout);

    /***********************************************************/
    /* Check to see if the poll call failed.                   */
    /***********************************************************/
    if (rc < 0) {
      perror("  poll() failed");
      return; // @todo: break/exit?
    }

    /***********************************************************/
    /* Check to see if the 3 minute time out expired.          */
    /***********************************************************/
    if (rc == 0) {
      return; // @todo: break/exit?
    }


    /***********************************************************/
    /* One or more descriptors are readable.  Need to          */
    /* determine which ones they are.                          */
    /***********************************************************/
    current_size = nfds;
    for (i = 0; i < current_size; i++) {
      /*********************************************************/
      /* Loop through to find the descriptors that returned    */
      /* POLLIN and determine whether it's the listening       */
      /* or the active connection.                             */
      /*********************************************************/
      if(fds[i].revents == 0) {
        continue;
      }

      /*********************************************************/
      /* If revents is not POLLIN, it's an unexpected result,  */
      /* log and end the server.                               */
      /*********************************************************/
      if(fds[i].revents != POLLIN) {
        glogf("Error! revents = %d", fds[i].revents);
        end_server = TRUE;
        break;
      }

      if (fds[i].fd == listen_sd) {
        /*******************************************************/
        /* Listening descriptor is readable.                   */
        /*******************************************************/
        glog("Listening socket is readable");

        /*******************************************************/
        /* Accept all incoming connections that are            */
        /* queued up on the listening socket before we         */
        /* loop back and call poll again.                      */
        /*******************************************************/
        do {
          /*****************************************************/
          /* Accept each incoming connection. If              */
          /* accept fails with EWOULDBLOCK, then we            */
          /* have accepted all of them. Any other             */
          /* failure on accept will cause us to end the        */
          /* server.                                           */
          /*****************************************************/
          new_sd = accept(listen_sd, NULL, NULL);
          if (new_sd < 0) {
            if (errno != EWOULDBLOCK) {
              perror("  accept() failed");
              end_server = TRUE;
            }
            break;
          }

          /*****************************************************/
          /* Add the new incoming connection to the            */
          /* pollfd structure                                  */
          /*****************************************************/
          glogf("New incoming connection - %d", new_sd);
          fds[nfds].fd = new_sd;
          fds[nfds].events = POLLIN;
          nfds++;

          // just set it and forget it :P
          debugger_sd = new_sd;
          timeout = 0;

          /*****************************************************/
          /* Loop back up and accept another incoming          */
          /* connection                                        */
          /*****************************************************/
        } while (new_sd != -1);
      }

      /*********************************************************/
      /* This is not the listening socket, therefore an        */
      /* existing connection must be readable                  */
      /*********************************************************/

      else {
        //printf("  Descriptor %d is readable\n", fds[i].fd);
        close_conn = FALSE;
        /*******************************************************/
        /* Receive all incoming data on this socket            */
        /* before we loop back and call poll again.            */
        /*******************************************************/

        do {
          /*****************************************************/
          /* Receive data on this connection until the         */
          /* recv fails with EWOULDBLOCK. If any other        */
          /* failure occurs, we will close the                 */
          /* connection.                                       */
          /*****************************************************/
          rc = recv(fds[i].fd, buffer, sizeof(buffer), MSG_DONTWAIT);
          if (rc < 0) {
            if (errno != EWOULDBLOCK) {
              perror("recv() failed");
              close_conn = TRUE;
            }
            break;
          }


          /*****************************************************/
          /* Check to see if the connection has been           */
          /* closed by the client                              */
          /*****************************************************/
          if (rc == 0) {
            glog("Connection closed\n");
            close_conn = TRUE;
            end_server = TRUE;
            break;
          }

          /*****************************************************/
          /* Data was received                                 */
          /*****************************************************/
          len = rc;
          char *mesg_ptr = buffer;
          char *split_ptr = strchr(mesg_ptr, '\n');

          int mesg_len = len-1;         // stripping that first char
          if(split_ptr) {
            int index = split_ptr - buffer;
            mesg_len = index - 1;       // stripping that first char
          }
          int debug_echo = FALSE;
          while (mesg_ptr < buffer + len - 1) {
            // DO DEBUG QUEUE
            if (dbg_cmd_queue_len < DBG_CMD_QUEUE_MAXLEN) {
              switch (mesg_ptr[0]) {
                case '0':
                  debug_push_message(G_DBG_COMMAND_TEST, mesg_ptr+1, mesg_len);
                  break;
                case '1':
                  debug_push_message(G_DBG_COMMAND_HELLO, mesg_ptr+1, mesg_len);
                  break;
                case '2':
                  debug_push_message(G_DBG_COMMAND_STEP, mesg_ptr+1, mesg_len);
                  break;
                case '3':
                  debug_push_message(G_DBG_COMMAND_CONTINUE, mesg_ptr+1, mesg_len);
                  break;
                case '4':
                  debug_push_message(G_DBG_COMMAND_GET_CPU, mesg_ptr+1, mesg_len);
                  break;
                case '5':
                  debug_push_message(G_DBG_COMMAND_SET_CPU, mesg_ptr+1, mesg_len);
                  break;
                case '6':
                  debug_push_message(G_DBG_COMMAND_GET_MEM, mesg_ptr+1, mesg_len);
                  break;
                case '7':
                  debug_push_message(G_DBG_COMMAND_SET_MEM, mesg_ptr+1, mesg_len);
                  break;
                case '8':
                  debug_push_message(G_DBG_COMMAND_ADD_BRK, mesg_ptr+1, mesg_len);
                  break;
                case '9':
                  debug_push_message(G_DBG_COMMAND_DEL_BRK, mesg_ptr+1, mesg_len);
                  break;
                case 'a':
                  debug_push_message(G_DBG_COMMAND_GET_BRK, mesg_ptr+1, mesg_len);
                  break;

                case 'b':               // DEPRECATED
                  debug_push_message(G_DBG_COMMAND_PAUSE, mesg_ptr+1, mesg_len);
                  break;
                case 'c':               // DEPRECATED
                  debug_push_message(G_DBG_COMMAND_DEBUGGER, mesg_ptr+1, mesg_len);
                  break;
                case 'd':               // DEPRECATED ????
                  debug_push_message(G_DBG_COMMAND_QUIT, mesg_ptr+1, mesg_len);
                  break;

                case 'e':
                  debug_push_message(G_DBG_COMMAND_EMU_CMD, mesg_ptr+1, mesg_len);
                  break;

                default:
                  glog("UNKNOWN COMMAND - DISCARDED");
              }


            } else {
              glog("COMMAND QUEUE FULL!  ABORT!");
              // @TODO probably send error response
            }

            mesg_ptr += mesg_len + 2;   // +1 for command char and +1 for '\n'
            split_ptr = strchr(mesg_ptr, '\n');
            if(split_ptr) {
              int index = split_ptr - mesg_ptr;
              mesg_len = index - 1;     // stripping that first char
            }
          }
          /*****************************************************/
          /* Echo the data back to the client                  */
          /*****************************************************/
          if (debug_echo) {
            rc = send(fds[i].fd, buffer, len, 0);
            if (rc < 0) {
              perror("  send() failed");
              close_conn = TRUE;
              break;
            }
          }

          // clear our buffer
          memset(buffer, 0, sizeof(buffer));

        } while(TRUE);

        /*******************************************************/
        /* If the close_conn flag was turned on, we need       */
        /* to clean up this active connection. This           */
        /* clean up process includes removing the              */
        /* descriptor.                                         */
        /*******************************************************/
        if (close_conn) {
          close(fds[i].fd);
          fds[i].fd = -1;
          compress_array = TRUE;
        }

      }  /* End of existing connection is readable             */
    } /* End of loop through pollable descriptors              */

    /***********************************************************/
    /* If the compress_array flag was turned on, we need       */
    /* to squeeze together the array and decrement the number  */
    /* of file descriptors. We do not need to move back the    */
    /* events and revents fields because the events will always*/
    /* be POLLIN in this case, and revents is output.          */
    /***********************************************************/
    if (compress_array) {
      compress_array = FALSE;
      for (i = 0; i < nfds; i++) {
        if (fds[i].fd == -1) {
          for(j = i; j < nfds; j++) {
            fds[j].fd = fds[j+1].fd;
          }
          nfds--;
        }
      }
    }
  } else {
    // end_server == TRUE   // @TODO handle server exit (and multiple clients?)
    /*************************************************************/
    /* Clean up all of the sockets that are open                  */
    /*************************************************************/
    for (i = 0; i < nfds; i++) {
      if(fds[i].fd >= 0)
        close(fds[i].fd);
    }
    nfds = 0;
  }
}


int do_dis_json(char *buf, word32 kpc, int accsize, int xsize, int op_provided, word32 instr, int chain) {
  char buf_instructions[5*5]; // '["12","DE","AB"]'
  char buf_disasm[50];


  const char *out;
  int args, type;
  int opcode;
  word32 val;
  word32 oldkpc;
  word32 dtype;
  int signed_val;

  oldkpc = kpc;
  if(op_provided) {
    opcode = (instr >> 24) & 0xff;
  } else {
    opcode = (int)get_memory_c(kpc, 0) & 0xff;
  }

  kpc++;

  dtype = disasm_types[opcode];
  out = disasm_opcodes[opcode];
  type = dtype & 0xff;
  args = dtype >> 8;

  if(args > 3) {
    if(args == 4) {
      args = accsize;
    } else if(args == 5) {
      args = xsize;
    }
  }

  val = -1;
  switch(args) {
    case 0:
      val = 0;
      break;
    case 1:
      if(op_provided) {
        val = instr & 0xff;
      } else {
        val = get_memory_c(kpc, 0);
      }
      break;
    case 2:
      if(op_provided) {
        val = instr & 0xffff;
      } else {
        val = get_memory16_c(kpc, 0);
      }
      break;
    case 3:
      if(op_provided) {
        val = instr & 0xffffff;
      } else {
        val = get_memory24_c(kpc, 0);
      }
      break;
    default:
      fprintf(stderr, "args out of range: %d, opcode: %08x\n",
              args, opcode);
      break;
  }
  kpc += args;

  if(!op_provided) {
    instr = (opcode << 24) | (val & 0xffffff);
  }

  switch(type) {
    case IMPLIED:
      sprintf(buf_disasm,"%s",out);
      break;
    case DP:
    case INTERRUPT:
      sprintf(buf_disasm,"%s   $%02x",out,val);
      break;
    case DP_X:
      sprintf(buf_disasm,"%s   $%02x,X",out,val);
      break;
    case DP_Y:
      sprintf(buf_disasm,"%s   $%02x,Y",out,val);
      break;
    case ABS:
      sprintf(buf_disasm,"%s   $%04x",out,val);
      break;
    case ABS_X:
      sprintf(buf_disasm,"%s   $%04x,X",out,val);
      break;
    case ABS_Y:
      sprintf(buf_disasm,"%s   $%04x,Y",out,val);
      break;
    case ABS_LONG:
      sprintf(buf_disasm,"%s   $%06x",out,val);
      break;
    case ABS_LONG_X:
      sprintf(buf_disasm,"%s   $%06x,X",out,val);
      break;

    case INDIR:
      sprintf(buf_disasm,"%s   ($%0*x)",out,args*2,val);
      break;
    case INDIR_X:
      sprintf(buf_disasm,"%s   ($%0*x,X)",out,args*2,val);
      break;
    case INDIR_Y:
      sprintf(buf_disasm,"%s   ($%0*x),Y",out,args*2,val);
      break;

    case INDIR_LONG:
      sprintf(buf_disasm,"%s   [$%0*x]",out,args*2,val);
      break;
    case INDIR_LONG_Y:
      sprintf(buf_disasm,"%s   [$%0*x],Y",out,args*2,val);
      break;

    case IMMEDIATE:
      sprintf(buf_disasm,"%s   #$%0*x",out, args*2, val);
      break;

    case SR:
      sprintf(buf_disasm,"%s   $%02x,S",out,val);
      break;

    case SR_Y:
      sprintf(buf_disasm,"%s   ($%02x,S),Y",out,val);
      break;

    case RELATIVE:
      signed_val = args == 1 ? (signed char)val : (signed short)val;

      sprintf(buf_disasm,"%s   $%04x",out,
              (word32)(kpc+(signed_val)) & 0xffff);
      break;

    case BLOCK:
      sprintf(buf_disasm,"%s   $%02x,$%02x",out,val>>8, val&0xff);
      break;

    default:
      printf("argument type: %d unexpected\n", type);
      break;
  }


  // gross
  word32 operand = instr;
  opcode = (operand >> 24) & 0xff;
  switch (args+1) {
    case 1:
      snprintf(buf_instructions, sizeof(buf_instructions),"[\"%02X\"]", opcode);
      break;
    case 2:
      snprintf(buf_instructions, sizeof(buf_instructions),"[\"%02X\",\"%02X\"]", opcode, instr & 0xff);
      break;
    case 3:
      snprintf(buf_instructions, sizeof(buf_instructions),"[\"%02X\",\"%02X\",\"%02X\"]", opcode, instr & 0xff, (instr & 0xff00) >> 8);
      break;
    case 4:
      snprintf(buf_instructions, sizeof(buf_instructions),"[\"%02X\",\"%02X\",\"%02X\",\"%02X\"]", opcode, instr & 0xff, (instr & 0xff00) >> 8, (instr & 0xff0000) >> 16);
      break;
    default:
      break;
  }


  // @TODO: FIX!!! NEEDS REAL BUFFER SIZE, note magic 1024
  snprintf(buf, 1024,"{\"type\":\"dis\",\"data\":{\"K\":\"%02X\",\"PC\":\"%04X\",\"bytes\":%s," \
           "\"disasm\":\"%s\",\"chain\":\"%d\"}}",
           oldkpc>>16, oldkpc & 0xffff,buf_instructions, buf_disasm, chain);
  return(args+1);
}

// BASE 64 ENCODER (FOR MEMORY DUMPS)
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64encode_len(int len) {
  return ((len + 2) / 3 * 4) + 1;
}

int b64encode(char *encoded, const char *string, int len) {
  int i;
  char *p;

  p = encoded;
  // encode a chunk of 4 chars
  for (i = 0; i < len - 2; i += 3) {
    *p++ = b64chars[(string[i] >> 2) & 0x3F];
    *p++ = b64chars[((string[i] & 0x3) << 4) | ((int) (string[i + 1] & 0xF0) >> 4)];
    *p++ = b64chars[((string[i + 1] & 0xF) << 2) | ((int) (string[i + 2] & 0xC0) >> 6)];
    *p++ = b64chars[string[i + 2] & 0x3F];
  }
  // end chunk
  if (i < len) {
    *p++ = b64chars[(string[i] >> 2) & 0x3F];

    if (i == (len - 1)) {
      *p++ = b64chars[((string[i] & 0x3) << 4)];
      *p++ = '=';
    } else {
      *p++ = b64chars[((string[i] & 0x3) << 4) |
                      ((int) (string[i + 1] & 0xF0) >> 4)];
      *p++ = b64chars[((string[i + 1] & 0xF) << 2)];
    }
    *p++ = '=';
  }
  // terminator
  *p++ = '\0';
  //printf("ENCODED : %d\n", p-encoded);
  return p - encoded;
}
