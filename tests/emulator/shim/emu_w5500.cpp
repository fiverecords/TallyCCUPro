/* emu_w5500.cpp - WIZnet W5500 emulated at the SPI/register level.
 *
 * The Arduino Ethernet 2.x library is compiled UNMODIFIED (utility/w5100.cpp
 * included). Its SPI frames arrive here while CS (pin 10) is LOW.
 * Frame (VDM): addr_hi, addr_lo, control = BSB[7:3] RWB[2] OM[1:0], data...
 *   BSB 0 common regs; 4n+1 socket n regs; 4n+2 TX buf; 4n+3 RX buf (2 KB).
 *
 * Every chip socket in TCP mode is backed by a non-blocking host socket.
 * The model is polled (no thread): reads of Sn_SR, Sn_IR, Sn_TX_FSR,
 * Sn_RX_RSR service the socket, plus a global pass at most every
 * EMU_W5500_TICK_US (default 500 us) of chip activity.
 *
 * Fidelity:
 *  - W5500 common register map (RTR 0x19, RCR 0x1B, VERSIONR=4, PHYCFGR
 *    link up). The Ethernet library writes RTR/RCR at the W5100 addresses
 *    (0x17/0x19) and that lands where it lands, as on the real chip.
 *  - Retransmission bound = sum_{i=0..RCR} min(RTR*2^i, 0xFFFF) x 100 us,
 *    applied to host sockets as TCP_USER_TIMEOUT and to SYN/FIN waits.
 *  - LISTEN: host listener on EMU_BIND_IP:port; a pending connection goes to
 *    the lowest LISTEN socket of that port; with none it is reset (chip
 *    answers SYN with RST). EMU_STRICT_LISTEN=0 queues instead.
 *  - CLOSE drops the connection silently (orphan); the peer gets RST when it
 *    sends something or closes.
 *  - CONNECT destination remap: EMU_REMAP="a.b.c.d=w.x.y.z,..." or
 *    EMU_REMAP_ALL. Sn_DIPR keeps the address the firmware wrote.
 *  - UDP data path not emulated (firmware does not use it).
 *  - Network trace (JSON lines) in EMU_NET_LOG.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <vector>
#include "emu_internal.h"
#include <time.h>
static unsigned long long wall_ms() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (unsigned long long)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000; }

namespace {

enum : uint8_t {
  SR_CLOSED = 0x00, SR_INIT = 0x13, SR_LISTEN = 0x14, SR_SYNSENT = 0x15,
  SR_ESTABLISHED = 0x17, SR_FIN_WAIT = 0x18, SR_TIME_WAIT = 0x1B,
  SR_CLOSE_WAIT = 0x1C, SR_LAST_ACK = 0x1D, SR_UDP = 0x22
};
enum : uint8_t { IR_CON = 0x01, IR_DISCON = 0x02, IR_RECV = 0x04, IR_TIMEOUT = 0x08, IR_SENDOK = 0x10 };
enum : uint8_t { CR_OPEN = 0x01, CR_LISTEN = 0x02, CR_CONNECT = 0x04, CR_DISCON = 0x08,
                 CR_CLOSE = 0x10, CR_SEND = 0x20, CR_SEND_KEEP = 0x22, CR_RECV = 0x40 };

const int NSOCK = 8;
const uint16_t BSZ = 2048;
const uint16_t BMASK = BSZ - 1;

struct Sock {
  uint8_t reg[0x30];
  uint8_t sr, ir;
  int fd;
  uint16_t tx_rd;         /* bytes handed to the kernel */
  uint16_t tx_end;        /* TX_WR latched at last SEND */
  bool sending;
  uint16_t rx_wr;         /* chip write pointer into RX buffer */
  uint16_t rx_rd;         /* RX_RD latched at last RECV */
  bool peer_fin;
  bool blackhole;         /* CONNECT to a host-unroutable address: no answer (like a PC that is off) */
  uint64_t t_state;
  uint8_t tx[BSZ];
  uint8_t rx[BSZ];
};

Sock S[NSOCK];
uint8_t C[0x40];

int f_phase;
uint16_t f_addr;
uint8_t f_ctrl;
int f_pending_cmd_sock = -1;
bool rtr_dirty;
bool lib_rtr_fix = false;   /* EMU_W5500_LIB_RTR_FIX=1: what-if the library wrote RTR/RCR at the W5500 addresses */

struct Listener { uint16_t port; int fd; };
std::vector<Listener> LS;
std::vector<int> ORPH;

bool cfg_done;
in_addr bind_ip;
struct Remap { in_addr from, to; };
std::vector<Remap> remaps;
bool remap_all_set;
in_addr remap_all;
bool strict_listen = true;
bool verbose;
int netlog_fd = -1;
uint64_t tick_ns, last_tick;
unsigned long stat_refused, stat_rst_orphan;

inline uint16_t g16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void netlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void netlog(const char *fmt, ...) {
  if (netlog_fd < 0) return;
  char buf[600];
  int n = snprintf(buf, sizeof buf, "{\"wt\":%llu,\"t\":%u,\"boot\":%d,", wall_ms(), emu_millis32(), emu_boot_count());
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(buf + n, sizeof buf - n - 3, fmt, ap);
  va_end(ap);
  if (m < 0) m = 0;
  n += m;
  if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
  buf[n++] = '}';
  buf[n++] = '\n';
  emu_write_line(netlog_fd, buf, n);
}

void common_defaults() {
  memset(C, 0, sizeof C);
  p16(&C[0x19], 0x07D0);   /* RTR 200 ms */
  C[0x1B] = 0x08;          /* RCR */
  C[0x1C] = 0x28; C[0x1D] = 0x28;
  C[0x2E] = 0xBF;          /* PHYCFGR: link up */
  C[0x39] = 0x04;          /* VERSIONR */
}

void sock_defaults(int s) {
  Sock &k = S[s];
  memset(k.reg, 0, sizeof k.reg);
  p16(&k.reg[0x12], 0x05B4);
  k.reg[0x16] = 0x80;
  k.reg[0x1E] = 2; k.reg[0x1F] = 2;
  p16(&k.reg[0x2D], 0x4000);
  k.reg[0x2C] = 0xFF;
  k.sr = SR_CLOSED; k.ir = 0;
  k.tx_rd = k.tx_end = 0; k.sending = false;
  k.rx_wr = k.rx_rd = 0; k.peer_fin = false; k.blackhole = false;
}

void config() {
  if (cfg_done) return;
  cfg_done = true;
  const char *b = emu_env("EMU_BIND_IP");
  if (!b || !inet_aton(b, &bind_ip)) inet_aton("127.0.0.1", &bind_ip);
  const char *r = emu_env("EMU_REMAP");
  if (r) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", r);
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
      char *eq = strchr(t, '=');
      if (!eq) continue;
      *eq = 0;
      Remap m;
      if (inet_aton(t, &m.from) && inet_aton(eq + 1, &m.to)) remaps.push_back(m);
    }
  }
  const char *ra = emu_env("EMU_REMAP_ALL");
  if (ra && inet_aton(ra, &remap_all)) remap_all_set = true;
  strict_listen = emu_env_bool("EMU_STRICT_LISTEN", 1);
  lib_rtr_fix = emu_env_bool("EMU_W5500_LIB_RTR_FIX", 0);
  verbose = emu_env_bool("EMU_VERBOSE", 0);
  netlog_fd = emu_open_log("EMU_NET_LOG");
  tick_ns = (uint64_t)emu_env_long("EMU_W5500_TICK_US", 500) * 1000ull;
  common_defaults();
  for (int s = 0; s < NSOCK; s++) { S[s].fd = -1; sock_defaults(s); }
}

uint32_t timeout_ms() {
  uint64_t tot = 0, cur = g16(&C[0x19]);
  uint32_t rcr = C[0x1B];
  for (uint32_t i = 0; i <= rcr; i++) {
    tot += cur;
    cur *= 2;
    if (cur > 0xFFFF) cur = 0xFFFF;
  }
  uint64_t ms = tot / 10;
  if (ms < 1) ms = 1;
  return (uint32_t)ms;
}

void apply_user_timeout(int fd) {
  unsigned int t = timeout_ms();
  setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &t, sizeof t);
}

void set_sock_opts(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int sz = (int)emu_env_long("EMU_SNDBUF", 4096);
  if (sz > 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
  int rz = (int)emu_env_long("EMU_RCVBUF", 4096);
  if (rz > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rz, sizeof rz);
  apply_user_timeout(fd);
}

void rst_close(int fd) {
  struct linger lg = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  close(fd);
}

void set_sr(int s, uint8_t sr, const char *why) {
  if (S[s].sr == sr) return;
  netlog("\"ev\":\"sr\",\"s\":%d,\"from\":%u,\"to\":%u,\"why\":\"%s\"", s, S[s].sr, sr, why);
  S[s].sr = sr;
  S[s].t_state = emu_mono_ms();
}

void drop_fd(int s, bool orphan) {
  Sock &k = S[s];
  if (k.fd < 0) return;
  if (orphan) ORPH.push_back(k.fd);
  else close(k.fd);
  k.fd = -1;
}

Listener *get_listener(uint16_t port) {
  for (size_t i = 0; i < LS.size(); i++) if (LS[i].port == port) return &LS[i];
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return NULL;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr = bind_ip;
  if (bind(fd, (sockaddr *)&a, sizeof a) < 0 || listen(fd, 16) < 0) {
    emu_log("W5500: cannot listen on %s:%u: %s", inet_ntoa(bind_ip), port, strerror(errno));
    close(fd);
    return NULL;
  }
  Listener l = {port, fd};
  LS.push_back(l);
  netlog("\"ev\":\"listen_host\",\"port\":%u", port);
  return &LS.back();
}

void service_listeners() {
  for (size_t i = 0; i < LS.size(); i++) {
    Listener &l = LS[i];
    for (;;) {
      int target = -1;
      for (int s = 0; s < NSOCK; s++)
        if (S[s].sr == SR_LISTEN && g16(&S[s].reg[0x04]) == l.port) { target = s; break; }
      if (target < 0 && !strict_listen) break;
      sockaddr_in pa;
      socklen_t pl = sizeof pa;
      int fd = accept4(l.fd, (sockaddr *)&pa, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (fd < 0) break;
      if (target < 0) {
        stat_refused++;
        netlog("\"ev\":\"refused\",\"port\":%u,\"peer\":\"%s:%u\"", l.port, inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
        emu_log("W5500: no LISTEN socket on port %u: connection from %s:%u reset", l.port, inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
        rst_close(fd);
        continue;
      }
      Sock &k = S[target];
      set_sock_opts(fd);
      k.fd = fd;
      memcpy(&k.reg[0x0C], &pa.sin_addr.s_addr, 4);
      p16(&k.reg[0x10], ntohs(pa.sin_port));
      k.ir |= IR_CON;
      set_sr(target, SR_ESTABLISHED, "accept");
      netlog("\"ev\":\"accept\",\"s\":%d,\"port\":%u,\"peer\":\"%s:%u\"", target, l.port, inet_ntoa(pa.sin_addr), ntohs(pa.sin_port));
    }
  }
}

void service_orphans() {
  for (size_t i = 0; i < ORPH.size();) {
    char b[256];
    ssize_t n = recv(ORPH[i], b, sizeof b, MSG_DONTWAIT);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
      stat_rst_orphan++;
      netlog("\"ev\":\"orphan_rst\",\"n\":%d", (int)n);
      rst_close(ORPH[i]);
      ORPH.erase(ORPH.begin() + i);
      continue;
    }
    i++;
  }
}

void fail_conn(int s, uint8_t irbits, const char *why) {
  Sock &k = S[s];
  int e = errno;
  netlog("\"ev\":\"conn_fail\",\"s\":%d,\"why\":\"%s\",\"err\":\"%s\"", s, why, strerror(e));
  if (verbose) emu_log("W5500: socket %d %s: %s", s, why, strerror(e));
  drop_fd(s, false);
  k.ir |= irbits;
  k.sending = false;
  set_sr(s, SR_CLOSED, why);
}

void service_sock(int s) {
  Sock &k = S[s];
  if (k.fd < 0 && k.blackhole && k.sr == SR_SYNSENT) {
    /* chip: ARP request retried RCR times every RTR, then TIMEOUT */
    uint64_t arp_ms = (uint64_t)g16(&C[0x19]) * (C[0x1B] + 1) / 10;
    if (emu_mono_ms() - k.t_state > arp_ms) {
      k.blackhole = false;
      errno = ETIMEDOUT;
      fail_conn(s, IR_TIMEOUT, "arp_timeout");
    }
    return;
  }
  if (k.fd < 0) {
    if (k.sr == SR_FIN_WAIT || k.sr == SR_LAST_ACK || k.sr == SR_TIME_WAIT || k.sr == SR_SYNSENT ||
        k.sr == SR_ESTABLISHED || k.sr == SR_CLOSE_WAIT)
      set_sr(s, SR_CLOSED, "nofd");
    return;
  }
  if (k.sr == SR_SYNSENT) {
    pollfd pf = {k.fd, POLLOUT, 0};
    int r = poll(&pf, 1, 0);
    if (r > 0) {
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(k.fd, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err) { errno = err; fail_conn(s, IR_TIMEOUT, "connect_err"); return; }
      k.ir |= IR_CON;
      set_sr(s, SR_ESTABLISHED, "connected");
    } else if (emu_mono_ms() - k.t_state > timeout_ms()) {
      errno = ETIMEDOUT;
      fail_conn(s, IR_TIMEOUT, "syn_timeout");
      return;
    } else {
      return;
    }
  }
  if (k.sr != SR_ESTABLISHED && k.sr != SR_CLOSE_WAIT && k.sr != SR_FIN_WAIT && k.sr != SR_LAST_ACK) return;

  /* TX */
  while (k.sending && k.tx_rd != k.tx_end) {
    uint16_t len = (uint16_t)(k.tx_end - k.tx_rd);
    uint16_t off = k.tx_rd & BMASK;
    uint16_t chunk = (uint16_t)(BSZ - off);
    if (chunk > len) chunk = len;
    ssize_t n = send(k.fd, k.tx + off, chunk, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n > 0) { k.tx_rd = (uint16_t)(k.tx_rd + n); continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    fail_conn(s, errno == ETIMEDOUT ? IR_TIMEOUT : IR_DISCON, "send_err");
    return;
  }
  if (k.sending && k.tx_rd == k.tx_end) { k.sending = false; k.ir |= IR_SENDOK; }

  /* RX */
  if (!k.peer_fin) {
    for (;;) {
      uint16_t used = (uint16_t)(k.rx_wr - k.rx_rd);
      uint16_t freeb = (uint16_t)(BSZ - used);
      if (!freeb) break;
      uint16_t off = k.rx_wr & BMASK;
      uint16_t chunk = (uint16_t)(BSZ - off);
      if (chunk > freeb) chunk = freeb;
      ssize_t n = recv(k.fd, k.rx + off, chunk, MSG_DONTWAIT);
      if (n > 0) { k.rx_wr = (uint16_t)(k.rx_wr + n); k.ir |= IR_RECV; continue; }
      if (n == 0) {
        k.peer_fin = true;
        k.ir |= IR_DISCON;
        netlog("\"ev\":\"peer_fin\",\"s\":%d", s);
        if (k.sr == SR_ESTABLISHED) set_sr(s, SR_CLOSE_WAIT, "peer_fin");
        else if (k.sr == SR_FIN_WAIT) { drop_fd(s, false); set_sr(s, SR_CLOSED, "fin_fin"); return; }
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      fail_conn(s, errno == ETIMEDOUT ? IR_TIMEOUT : IR_DISCON, "recv_err");
      return;
    }
  } else if (k.sr == SR_FIN_WAIT) {
    drop_fd(s, false);
    set_sr(s, SR_CLOSED, "fin_done");
    return;
  }
  if (k.sr == SR_FIN_WAIT || k.sr == SR_LAST_ACK) {
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(k.fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { errno = err; fail_conn(s, IR_TIMEOUT, "fin_err"); return; }
    if (k.sr == SR_LAST_ACK) {
      int outq = 0;
      ioctl(k.fd, SIOCOUTQ, &outq);
      if (outq == 0) { drop_fd(s, false); set_sr(s, SR_CLOSED, "last_ack"); return; }
    }
    if (emu_mono_ms() - k.t_state > timeout_ms()) {
      errno = ETIMEDOUT;
      fail_conn(s, IR_TIMEOUT, "fin_timeout");
      return;
    }
  }
}

void tick(bool force) {
  uint64_t now = emu_mono_ns();
  if (!force && now - last_tick < tick_ns) return;
  last_tick = now;
  service_listeners();
  service_orphans();
  for (int s = 0; s < NSOCK; s++) service_sock(s);
}

void do_cmd(int s, uint8_t cmd) {
  Sock &k = S[s];
  uint8_t mr = k.reg[0x00] & 0x0F;
  if (cmd != CR_SEND && cmd != CR_RECV)
    netlog("\"ev\":\"cmd\",\"s\":%d,\"cmd\":%u,\"sr\":%u,\"port\":%u", s, cmd, k.sr, g16(&k.reg[0x04]));
  switch (cmd) {
    case CR_OPEN:
      drop_fd(s, true);
      k.blackhole = false;
      k.tx_rd = k.tx_end = 0; k.sending = false;
      k.rx_wr = k.rx_rd = 0; k.peer_fin = false;
      p16(&k.reg[0x24], 0); p16(&k.reg[0x28], 0);
      if (mr == 0x01) set_sr(s, SR_INIT, "open");
      else if (mr == 0x02) set_sr(s, SR_UDP, "open_udp");
      else set_sr(s, SR_CLOSED, "open_bad_mode");
      break;
    case CR_LISTEN:
      if (k.sr == SR_INIT) {
        get_listener(g16(&k.reg[0x04]));
        set_sr(s, SR_LISTEN, "listen");
      }
      break;
    case CR_CONNECT: {
      if (k.sr != SR_INIT) break;
      sockaddr_in a;
      memset(&a, 0, sizeof a);
      a.sin_family = AF_INET;
      memcpy(&a.sin_addr.s_addr, &k.reg[0x0C], 4);
      a.sin_port = htons(g16(&k.reg[0x10]));
      in_addr orig = a.sin_addr;
      bool mapped = false;
      for (size_t i = 0; i < remaps.size(); i++)
        if (remaps[i].from.s_addr == orig.s_addr) { a.sin_addr = remaps[i].to; mapped = true; break; }
      if (!mapped && remap_all_set) a.sin_addr = remap_all;
      int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      sockaddr_in la;
      memset(&la, 0, sizeof la);
      la.sin_family = AF_INET;
      la.sin_addr = bind_ip;
      if (bind(fd, (sockaddr *)&la, sizeof la) < 0) emu_log("W5500: bind for connect: %s", strerror(errno));
      set_sock_opts(fd);
      k.fd = fd;
      set_sr(s, SR_SYNSENT, "connect");
      char o[32];
      snprintf(o, sizeof o, "%s", inet_ntoa(orig));
      netlog("\"ev\":\"connect\",\"s\":%d,\"dst\":\"%s:%u\",\"via\":\"%s\"", s, o, ntohs(a.sin_port), inet_ntoa(a.sin_addr));
      int r = connect(fd, (sockaddr *)&a, sizeof a);
      if (r < 0 && errno != EINPROGRESS) {
        if ((errno == EINVAL || errno == ENETUNREACH || errno == EHOSTUNREACH) && !emu_env_bool("EMU_UNROUTABLE_REFUSE", 0)) {
          netlog("\"ev\":\"blackhole\",\"s\":%d", s);
          close(fd);
          k.fd = -1;
          k.blackhole = true;
        } else {
          fail_conn(s, IR_TIMEOUT, "connect_now");
        }
      }
      break;
    }
    case CR_DISCON:
      if (k.sr == SR_ESTABLISHED || k.sr == SR_CLOSE_WAIT) {
        service_sock(s);
        if (k.fd >= 0) shutdown(k.fd, SHUT_WR);
        if (k.sr == SR_ESTABLISHED) set_sr(s, SR_FIN_WAIT, "discon");
        else if (k.sr == SR_CLOSE_WAIT) set_sr(s, SR_LAST_ACK, "discon");
      } else if (k.sr == SR_SYNSENT || k.sr == SR_LISTEN || k.sr == SR_INIT) {
        k.blackhole = false;
        drop_fd(s, false);
        set_sr(s, SR_CLOSED, "discon_idle");
      }
      break;
    case CR_CLOSE:
      k.blackhole = false;
      if (k.fd >= 0) drop_fd(s, k.sr == SR_ESTABLISHED || k.sr == SR_CLOSE_WAIT);
      k.sending = false;
      set_sr(s, SR_CLOSED, "close");
      break;
    case CR_SEND:
      k.tx_end = g16(&k.reg[0x24]);
      k.sending = true;
      service_sock(s);
      break;
    case CR_RECV:
      k.rx_rd = g16(&k.reg[0x28]);
      service_sock(s);
      break;
    default:
      break;
  }
}

void soft_reset() {
  netlog("\"ev\":\"soft_reset\"");
  for (int s = 0; s < NSOCK; s++) { drop_fd(s, true); sock_defaults(s); }
  common_defaults();
}

void before_read(int bsb, uint16_t addr) {
  if (bsb == 0) { tick(false); return; }
  if ((bsb & 3) != 1) return;
  int s = bsb >> 2;
  if (s >= NSOCK) return;
  if (addr == 0x02 || addr == 0x03 || addr == 0x20 || addr == 0x26) {
    tick(false);
    service_sock(s);
  }
}

uint8_t read_sreg(int s, uint16_t a) {
  Sock &k = S[s];
  if (a >= 0x30) return 0;
  uint8_t b[2];
  switch (a) {
    case 0x01: return 0;
    case 0x02: return k.ir;
    case 0x03: return k.sr;
    case 0x20: case 0x21: {
      uint16_t fsr = (uint16_t)(BSZ - (uint16_t)(g16(&k.reg[0x24]) - k.tx_rd));
      p16(b, fsr); return b[a - 0x20];
    }
    case 0x22: case 0x23: p16(b, k.tx_rd); return b[a - 0x22];
    case 0x26: case 0x27: p16(b, (uint16_t)(k.rx_wr - k.rx_rd)); return b[a - 0x26];
    case 0x2A: case 0x2B: p16(b, k.rx_wr); return b[a - 0x2A];
    default: return k.reg[a];
  }
}

void write_sreg(int s, uint16_t a, uint8_t v) {
  Sock &k = S[s];
  if (a >= 0x30) return;
  switch (a) {
    case 0x01: f_pending_cmd_sock = s; k.reg[1] = v; return;
    case 0x02: k.ir &= (uint8_t)~v; return;
    case 0x03: case 0x20: case 0x21: case 0x22: case 0x23: case 0x26: case 0x27: case 0x2A: case 0x2B:
      return;
    default: k.reg[a] = v; return;
  }
}

uint8_t read_creg(uint16_t a) { return a < sizeof C ? C[a] : 0; }

void write_creg(uint16_t a, uint8_t v) {
  if (a >= sizeof C) return;
  if (lib_rtr_fix && a >= 0x17 && a <= 0x19) {
    /* W5100-map writes from setRetransmissionTime/Count redirected to the W5500 RTR/RCR */
    a = (uint16_t)(a + 2);
    rtr_dirty = true;
    C[a] = v;
    return;
  }
  if (a == 0x00) {
    if (v & 0x80) { soft_reset(); return; }
    C[0] = v;
    return;
  }
  if (a == 0x39) return;
  if (a == 0x2E) { C[a] = (uint8_t)((v & 0xF8) | (C[a] & 0x07)); return; }
  if (a == 0x15 || a == 0x17) { C[a] &= (uint8_t)~v; return; }
  if (a >= 0x19 && a <= 0x1B) rtr_dirty = true;
  C[a] = v;
}

}  // namespace

extern "C" void emu_w5500_cs(int selected) {
  config();
  if (selected) {
    f_phase = 0;
    f_pending_cmd_sock = -1;
    return;
  }
  if (rtr_dirty) {
    rtr_dirty = false;
    netlog("\"ev\":\"rtr\",\"rtr\":%u,\"rcr\":%u,\"timeout_ms\":%u", g16(&C[0x19]), C[0x1B], timeout_ms());
    emu_log("W5500: RTR=%u RCR=%u -> TCP retransmission bound %u ms", g16(&C[0x19]), C[0x1B], timeout_ms());
    for (int s = 0; s < NSOCK; s++) if (S[s].fd >= 0) apply_user_timeout(S[s].fd);
  }
  if (f_pending_cmd_sock >= 0) {
    int s = f_pending_cmd_sock;
    f_pending_cmd_sock = -1;
    do_cmd(s, S[s].reg[1]);
    S[s].reg[1] = 0;
  }
}

extern "C" uint8_t emu_w5500_spi(uint8_t mosi) {
  config();
  if (f_phase == 0) { f_addr = (uint16_t)(mosi << 8); f_phase = 1; return 0x01; }
  if (f_phase == 1) { f_addr |= mosi; f_phase = 2; return 0x02; }
  if (f_phase == 2) {
    f_ctrl = mosi;
    f_phase = 3;
    if (!(f_ctrl & 0x04)) before_read(f_ctrl >> 3, f_addr);
    return 0x03;
  }
  int bsb = f_ctrl >> 3;
  bool wr = (f_ctrl & 0x04) != 0;
  uint16_t a = f_addr++;
  uint8_t out = 0;
  if (bsb == 0) {
    if (wr) write_creg(a, mosi); else out = read_creg(a);
  } else {
    int s = bsb >> 2;
    int kind = bsb & 3;
    if (s >= NSOCK) return 0;
    Sock &k = S[s];
    if (kind == 1) { if (wr) write_sreg(s, a, mosi); else out = read_sreg(s, a); }
    else if (kind == 2) { if (wr) k.tx[a & BMASK] = mosi; else out = k.tx[a & BMASK]; }
    else if (kind == 3) { if (wr) k.rx[a & BMASK] = mosi; else out = k.rx[a & BMASK]; }
  }
  return out;
}

extern "C" void emu_w5500_prepare_exec(char *envbuf, size_t envbuf_size) {
  /* Host sockets are CLOEXEC: on the emulated MCU reset every connection is
   * closed by the host kernel. A real W5500 keeps them half-open until the
   * new firmware's soft reset (documented limitation). */
  if (envbuf && envbuf_size) envbuf[0] = 0;
  netlog("\"ev\":\"mcu_reset\",\"refused\":%lu,\"orphan_rst\":%lu", stat_refused, stat_rst_orphan);
}
