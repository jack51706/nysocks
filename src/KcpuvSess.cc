#include "KcpuvSess.h"
#include "utils.h"
#include <cassert>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace kcpuv {
// kpcuv_sess
static kcpuv_sess_list *sess_list = NULL;
static char *buffer = NULL;
// KcpuvSess extern
// TODO: it only accepts int in uv.h
long kcpuv_udp_buf_size = 4 * 1024 * 1024;

static short enable_timeout = KCPUV_SESS_TIMEOUT;

static int kcpuv__should_output_buf(unsigned int recvBufLength,
                                    unsigned int content_length) {
  return ((long)(recvBufLength + content_length)) >= KCPUV_SESS_BUFFER_SIZE;
}

static void kcpuv__replace_buf(KcpuvSess *sess, const char *content,
                               unsigned int content_length) {
  sess->recvBufLength = content_length;
  memcpy(sess->recvBuf, content, content_length);
}

static void kcpuv__put_buf(KcpuvSess *sess, const char *content,
                           unsigned int content_length) {
  memcpy(sess->recvBuf + sess->recvBufLength, content, content_length);
  sess->recvBufLength += content_length;
}

// void KcpuvPrintSessList_() {
//   if (sess_list == NULL) {
//     fprintf(stderr, "sess_list_length: 0\n");
//   }
//
//   kcpuv_link *link = sess_list->list;
//   int count = 0;
//
//   while (link != NULL) {
//     count++;
//     link = link->next;
//   }
//
//   fprintf(stderr, "sess_list_length: %d\n", count - 1);
// }

void KcpuvSess::KcpuvSessEnableTimeout(short value) { enable_timeout = value; }

// NOTE: Use this after first creation.
kcpuv_sess_list *KcpuvSess::KcpuvGetSessList() { return sess_list; }

void KcpuvSess::KcpuvInitialize() {
  if (sess_list != NULL) {
    return;
  }

  // check and create list
  sess_list = new kcpuv_sess_list;
  sess_list->list = kcpuv_link_create(NULL);
  sess_list->len = 0;

  // init buffer
  buffer = new char[BUFFER_LEN];
}

int KcpuvSess::KcpuvDestruct() {
  if (sess_list == NULL) {
    return 0;
  }

  if (buffer != NULL) {
    delete buffer;
    buffer = NULL;
  }

  kcpuv_link *ptr = sess_list->list;
  kcpuv_link *ptr_next = ptr->next;

  // destruct all nodes
  while (ptr_next != NULL) {
    KcpuvSess *sess = reinterpret_cast<KcpuvSess *>(ptr_next->node);
    // TODO: Inconsistency.
    delete sess;
    ptr_next = ptr->next;
  }

  delete sess_list->list;
  delete sess_list;
  sess_list = NULL;

  return 0;
}

bool KcpuvSess::AllowSend() {
  return state != KCPUV_STATE_WAIT_PASSIVELY && state < KCPUV_STATE_FIN_ACK;
}

bool KcpuvSess::AllowInput() { return state < KCPUV_STATE_FIN_ACK; }

// TODO: Allow outside to know if the operation is successful.
// Send raw data through kcp.
void KcpuvSess::KcpuvRawSend(KcpuvSess *sess, const int cmd, const char *msg,
                             unsigned long len) {
  if (!sess->AllowSend()) {
    if (KCPUV_DEBUG) {
      fprintf(stderr, "input with invalid state");
    }
    return;
  }

  sess->sendTs = iclock();

  // encode protocol
  // int write_len = len + KCPUV_OVERHEAD;
  //
  // // ikcp assume we copy and send the msg
  // char *plaintext = malloc(sizeof(char) * write_len);
  //
  // Cryptor::KcpuvProtocolEncode(cmd, plaintext);
  //
  // if (len != 0 && msg != NULL) {
  //   memcpy(plaintext + KCPUV_OVERHEAD, msg, len);
  // }

  // split content and send
  unsigned long s = 0;

  while (s == 0 || s < len) {
    // The position that we have send the data before it.
    unsigned long e = s + MAX_SENDING_LEN - KCPUV_OVERHEAD;

    if (e > len) {
      e = len;
    }

    unsigned long part_len = e - s;

    char *plaintext = new char[part_len + KCPUV_OVERHEAD];
    Cryptor::KcpuvProtocolEncode(cmd, plaintext);

    if (part_len != 0 && msg != NULL) {
      memcpy(plaintext + KCPUV_OVERHEAD, msg + s, part_len);
    }
    int rval = ikcp_send(sess->kcp, plaintext, part_len + KCPUV_OVERHEAD);

    if (KCPUV_DEBUG == 1 && rval < 0) {
      // TODO:
      printf("ikcp_send() < 0: %d", rval);
    }

    s = e;
    delete[] plaintext;

    if (s == 0) {
      break;
    }
  }
}

// Send app data through kcp.
void KcpuvSess::KcpuvSend(KcpuvSess *sess, const char *msg, unsigned long len) {
  KcpuvRawSend(sess, KCPUV_CMD_PUSH, msg, len);
}

// Func to output data for kcp through udp.
// NOTE: Should call `KcpuvInitSend` with the session before KcpOutput
// TODO: do not allocate twice
static int KcpOutput(const char *msg, int len, ikcpcb *kcp, void *user) {
  KcpuvSess *sess = (KcpuvSess *)user;

  if (!sess->AllowSend()) {
    fprintf(stderr, "%s\n", "output with invalid state");
    return -1;
  }

  if (KCPUV_DEBUG) {
    printf("output: %d %lld\n", len, iclock64());
    printf("content: ");
    print_as_hex(msg, len);
    printf("\n");
  }

  // encrypt
  char *data = reinterpret_cast<char *>(
      Cryptor::KcpuvCryptorEncrypt(sess->cryptor, (unsigned char *)msg, &len));

  sess->sessUDP->Send(data, len);

  return 0;
}

// // Send cmds.
void KcpuvSess::KcpuvSendCMD(KcpuvSess *sess, const int cmd) {
  KcpuvRawSend(sess, cmd, NULL, 0);
}

// Create a kcpuv session. This is a common structure for
// both of the sending and receiving.
// A session could only have one recvAddr and sendAddr.
KcpuvSess::KcpuvSess() {
  IUINT32 now = iclock();

  // KcpuvSess *sess = malloc(sizeof(KcpuvSess));
  kcp = ikcp_create(0, this);
  // NOTE: Support stream mode
  // kcp->stream = 1;
  // ikcp_nodelay(kcp, 0, 10, 0, 0);
  ikcp_nodelay(kcp, 1, 10, 2, 1);
  ikcp_wndsize(kcp, INIT_WND_SIZE, INIT_WND_SIZE);
  ikcp_setmtu(kcp, MTU_DEF);
  // kcp->rmt_wnd = INIT_WND_SIZE;

  // NOTE: uv_handle_t will be freed automatically
  sessUDP = new SessUDP(Loop::kcpuv_get_loop());
  sessUDP->data = this;

  recvBufLength = 0;
  recvBuf = new char[KCPUV_SESS_BUFFER_SIZE];
  mux = NULL;
  // TODO:
  // handle->data = this;
  recvAddr = NULL;
  onMsgCb = NULL;
  onCloseCb = NULL;
  state = KCPUV_STATE_CREATED;
  recvTs = now;
  sendTs = now;
  timeout = DEFAULT_TIMEOUT;
  cryptor = NULL;
  onBeforeFree = NULL;

  // set output func for kcp
  kcp->output = KcpOutput;

  // create link and push to the queue
  kcpuv_link *link = kcpuv_link_create(this);
  kcpuv_link_add(sess_list->list, link);

  sess_list->len += 1;
}

bool KcpuvSess::ExitUpdateQueue() {
  if (state == KCPUV_STATE_WAIT_FREE) {
    return 0;
  }

  state = KCPUV_STATE_WAIT_FREE;

  // Possible created without sess_list.
  if (sess_list == NULL || sess_list->list == NULL) {
    return 0;
  }

  kcpuv_link *ptr = kcpuv_link_get_pointer(sess_list->list, this);
  assert(ptr != NULL);
  delete ptr;
  sess_list->len -= 1;

  return 1;
}

// NOTE: Outside is expected to delete instances manually.
void KcpuvSess::TriggerClose() {
  if (onCloseCb != NULL) {
    // call callback to inform outside
    CloseCb cb = onCloseCb;
    cb(this);
  }
}

// Free a kcpuv session.
KcpuvSess::~KcpuvSess() {
  this->ExitUpdateQueue();

  if (cryptor != NULL) {
    Cryptor::KcpuvCryptorClean(cryptor);
    delete cryptor;
  }

  if (recvAddr != NULL) {
    delete recvAddr;
  }

  // TODO: Make sure sessUdp will be closed correctly.
  // if (!uv_is_closing((uv_handle_t *)handle)) {
  //   uv_close((uv_handle_t *)handle, free_handle_cb);
  // }
  delete sessUDP;

  // // TODO: should stop listening
  // if (handle->data != NULL) {
  //   // freed
  // } else if (!uv_is_closing((uv_handle_t *)handle)) {
  // } else {
  //   // handle->data = NULL;
  //   // free(handle);
  // }

  delete recvBuf;
  ikcp_release(kcp);
}

// TODO: export salt
void KcpuvSess::KcpuvSessInitCryptor(KcpuvSess *sess, const char *key,
                                     int len) {
  unsigned int salt[] = {1, 2};
  sess->cryptor = new kcpuv_cryptor;
  Cryptor::KcpuvCryptorInit(sess->cryptor, key, len, salt);
}

// Set sending info.
void KcpuvSess::KcpuvInitSend(KcpuvSess *sess, char *addr, int port) {
  sess->sessUDP->SetSendAddr(addr, port);
}

// TODO: remove this
// Update kcp for content transmission.
static int input_kcp(KcpuvSess *sess, const char *msg, int length) {
  int rval = ikcp_input(sess->kcp, msg, length);

  if (KCPUV_DEBUG == 1 && rval < 0) {
    // TODO:
    fprintf(stderr, "ikcp_input() < 0: %d\n", rval);
  }

  return rval;
}

// Input dgram mannualy.
// `len` is also `nread`.
void KcpuvSess::KcpuvInput(KcpuvSess *sess, const struct sockaddr *addr,
                           const char *data, int len) {
  if (!sess->AllowInput()) {
    fprintf(stderr, "%s\n", "invalid msg input");
    return;
  }

  if (len < 0) {
    // TODO:
    fprintf(stderr, "uv error: %s\n", uv_strerror(len));
  }

  if (len > 0) {
    // Mainly for the side that starts the conversation.
    if (sess->state == KCPUV_STATE_CREATED) {
      sess->state = KCPUV_STATE_READY;
    }

    // Mainlyy for the side waiting for a conversation.
    if (sess->state == KCPUV_STATE_WAIT_PASSIVELY) {
      sess->state = KCPUV_STATE_READY;

      if (!sess->sessUDP->HasSendAddr()) {
        sess->sessUDP->SetSendAddrBySockaddr(addr);
        // if (KCPUV_DEBUG) {
        //   kcpuv__print_sockaddr(addr);
        // }
      }
    }

    int read_len = len;
    char *read_msg = reinterpret_cast<char *>(Cryptor::KcpuvCryptorDecrypt(
        sess->cryptor, (unsigned char *)data, &read_len));

    // if (KCPUV_DEBUG) {
    //   print_as_hex(buf->base, nread);
    //   print_as_hex(read_msg, read_len);
    // }

    // update active time
    sess->recvTs = iclock();

    input_kcp(sess, read_msg, read_len);

    delete[] read_msg;
  }
}

// void KcpuvBindBeforeFree(KcpuvSess *sess, CloseCb onBeforeFree) {
//   onBeforeFree = onBeforeFree;
// }

// Called when uv receives a msg and pass.
static void OnDgramCb(SessUDP *sessUDP, const struct sockaddr *addr,
                      const char *data, int len) {
  KcpuvSess *sess = reinterpret_cast<KcpuvSess *>(sessUDP->data);
  KcpuvSess::KcpuvInput(sess, addr, data, len);
}

// Set receiving info.
int KcpuvSess::KcpuvListen(KcpuvSess *sess, int port, DataCb cb) {
  assert(sess->state == KCPUV_STATE_CREATED);
  sess->state = KCPUV_STATE_WAIT_PASSIVELY;
  sess->onMsgCb = cb;
  return sess->sessUDP->Bind(port, OnDgramCb);
}

// // Stop listening
// int KcpuvStopListen(KcpuvSess *sess) { return uv_udp_recv_stop(handle); }
//
// // Get address and port of a sess.
// int KcpuvGetAddress(KcpuvSess *sess, char *addr, int *namelen, int *port) {
//   // Assume their `sa_family`es are all ipv4.
//   struct sockaddr *name = malloc(sizeof(struct sockaddr));
//   *namelen = sizeof(struct sockaddr);
//   int rval = uv_udp_getsockname((const uv_udp_t *)handle,
//                                 (struct sockaddr *)name, namelen);
//
//   if (rval) {
//     free(name);
//     return rval;
//   }
//
//   if (name->sa_family == AF_INET) {
//     uv_ip4_name((const struct sockaddr_in *)name, addr, IP4_ADDR_LENTH);
//     *port = ntohs(((struct sockaddr_in *)name)->sin_port);
//   } else {
//     uv_ip6_name((const struct sockaddr_in6 *)name, addr, IP6_ADDR_LENGTH);
//     *port = ntohs(((struct sockaddr_in6 *)name)->sin6_port);
//   }
//
//   free(name);
//   return 0;
// }

// Set close msg listener
void KcpuvSess::KcpuvBindClose(KcpuvSess *sess, CloseCb cb) {
  assert(cb);
  sess->onCloseCb = cb;
}

// void KcpuvBindListen(KcpuvSess *sess, kcpuv_listen_cb cb) { onMsgCb = cb; }
//
// int KcpuvSetState(KcpuvSess *sess, int state) {
//   // do not allow to change state when it's KCPUV_STATE_WAIT_FREE
//   // if (state != KCPUV_STATE_WAIT_FREE && state == KCPUV_STATE_WAIT_FREE)
//   // {
//   //   return -1;
//   // }
//
//   state = state;
//   return 0;
// }

// Close
void KcpuvSess::KcpuvClose(KcpuvSess *sess) {
  if (sess->state >= KCPUV_STATE_FIN) {
    return;
  }

  sess->state = KCPUV_STATE_FIN;

  // TODO: what if a sess is not established?
  KcpuvSendCMD(sess, KCPUV_CMD_FIN);

  // mark that this sess could be freed
  // TODO: where to put KCPUV_STATE_CLOSED ?
  // state = KCPUV_STATE_CLOSED;

  // uv_close(handle, NULL);
}

static void TriggerBeforeClose(KcpuvSess *sess) {
  if (sess->onBeforeFree != NULL) {
    CloseCb cb = sess->onBeforeFree;
    cb(sess);
  }
}

// Iterate the session_list and update kcp
void KcpuvSess::KcpuvUpdateKcpSess_(uv_timer_t *timer) {
  if (!sess_list || !sess_list->list) {
    return;
  }

  kcpuv_link *ptr = sess_list->list;

  // TODO: maybe we could assume that ikcp_update won't
  // cost too much time and get the time once
  IUINT32 now = 0;

  while (ptr->next != NULL) {
    int size;

    if (!ptr->next->node) {
      assert(0);
    }

    KcpuvSess *sess = (KcpuvSess *)ptr->next->node;
    unsigned int timeout = sess->timeout;
    ikcpcb *kcp = sess->kcp;
    int state = sess->state;

    now = iclock();

    if (enable_timeout && timeout && now - sess->recvTs >= timeout &&
        sess->state < KCPUV_STATE_WAIT_FREE) {
      sess->ExitUpdateQueue();
      TriggerBeforeClose(sess);
      // TODO: Tell outside that it exits because of timeout.
      sess->TriggerClose();
      continue;
    }

    IUINT32 time_to_update = ikcp_check(kcp, now);

    // NOTE: We have to call ikcp_update after the first calling
    // of the ikcp_input or we will get Segmentation fault.
    if (time_to_update <= now) {
      ikcp_update(kcp, now);
    }

    size = ikcp_recv(kcp, buffer, BUFFER_LEN);

    // TODO: consider the expected size
    while (size > 0) {
      // print_as_hex((const char *)(buffer), size);
      // parse cmd
      // check protocol
      int cmd = Cryptor::KcpuvProtocolDecode(buffer);

      // NOTE: We don't need to make sure the KCPUV_CMD_NOO
      // to be received as messages sended by kcp will also
      // refresh the recvTs to avoid timeout.
      // NOTE: Some states need to be considered carefully.
      if (cmd == KCPUV_CMD_NOO) {
        // do nothing
      } else if (cmd == KCPUV_CMD_FIN) {
        if (state <= KCPUV_STATE_READY) {
          sess->state = KCPUV_STATE_FIN_ACK;
          KcpuvSendCMD(sess, KCPUV_CMD_FIN_ACK);
        }
      } else if (cmd == KCPUV_CMD_FIN_ACK) {
        sess->state = KCPUV_STATE_FIN_ACK;
      } else if (cmd == KCPUV_CMD_PUSH) {
        unsigned int content_length = size - KCPUV_OVERHEAD;
        // Push the packet into recvBuf.
        if (!kcpuv__should_output_buf(sess->recvBufLength, content_length)) {
          kcpuv__put_buf(sess, (const char *)(buffer + KCPUV_OVERHEAD),
                         content_length);
        } else {
          if (sess->onMsgCb != NULL) {
            // update receive data
            DataCb onMsgCb = onMsgCb;
            onMsgCb(sess, sess->recvBuf, sess->recvBufLength);
          }

          kcpuv__replace_buf(sess, buffer + KCPUV_OVERHEAD, content_length);
        }
      } else {
        // invalid CMD
        fprintf(stderr, "receive invalid cmd: %d\n", cmd);
      }

      size = ikcp_recv(kcp, buffer, BUFFER_LEN);
    }

    // TODO: Is call next function in the same tick makes it more efficient?
    if (sess->recvBufLength > 0) {
      if (sess->onMsgCb != NULL) {
        // update receive data
        DataCb onMsgCb = sess->onMsgCb;
        onMsgCb(sess, sess->recvBuf, sess->recvBufLength);
      }

      kcpuv__replace_buf(sess, NULL, 0);
    }

    if (size < 0) {
      // rval == -1 queue is empty
      // rval == -2 peeksize is zero(or invalid)
      if (size != -1 && size != -2) {
        // TODO:
        fprintf(stderr, "ikcp_recv() < 0: %d\n", size);
      }
    }

    ptr = ptr->next;
  }

  ptr = sess_list->list;

  while (ptr->next != NULL) {
    if (!ptr->next->node) {
      assert(0);
    }

    KcpuvSess *sess = (KcpuvSess *)ptr->next->node;

    if (sess->state == KCPUV_STATE_FIN_ACK) {
      int packets = ikcp_waitsnd(sess->kcp);

      // Trigger close when all data acked
      if (packets == 0) {
        sess->ExitUpdateQueue();
        TriggerBeforeClose(sess);
        // NOTE: kcpuv free will remove ptr
        sess->TriggerClose();
        continue;
      }
    } else {
      if (KCPUV_SESS_HEARTBEAT_ACTIVE && sess->sessUDP->HasSendAddr() &&
          sess->state == KCPUV_STATE_READY &&
          sess->sendTs + KCPUV_SESS_HEARTBEAT_INTERVAL <= now &&
          ikcp_waitsnd(sess->kcp) == 0) {
        KcpuvSendCMD(sess, KCPUV_CMD_NOO);
      }
    }

    // Might more than one sessions exit.
    ptr = ptr->next;
  }
}

void KcpuvSess::SetTimeout(unsigned int t) { timeout = t; }
} // namespace kcpuv
