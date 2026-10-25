#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sys/uio.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "logger.hpp"
#include "parser.hpp"

// Wire format (network byte order):
//   DATA: 1 | u8 sender | u8 k | k x (u32 seq | u16 len | payload)
//   ACK:  2 | u8 receiver | u32 low | window/32 x u32 mask   (every seq < low is delivered; bit i of the mask = seq low+1+i is delivered)
// The receiver sends one ACK per sender per receive burst, describing its whole state, so ACKs are cumulative and idempotent.
// Sender and receiver share the window size: the sender never has more than
// gWindow unacked seqs, so the receiver can track [low, low+gWindow) in a bitset.
namespace {

constexpr uint8_t kData = 1, kAck = 2;
constexpr uint32_t kMaxWindow = 256; // multiple of 32 (ACK bitmap words)
// Both sides derive the same window from the number of processes, see windowFor().
uint32_t gWindow = 32;
constexpr auto kRto = std::chrono::milliseconds(500);
constexpr auto kScanEvery = std::chrono::milliseconds(50);
constexpr size_t kMaxPacket = 65536;
constexpr unsigned kMaxBatch = 8; // messages per packet (project limit)
constexpr size_t kMaxAckSize = 6 + 4 * (kMaxWindow / 32);
size_t ackSize() { return 6 + 4 * size_t{gWindow / 32}; }

// The receiver socket buffer holds only ~200 small datagrams, so the total of in-flight packets
// ((n-1) senders x window/8) is budgeted; with few processes the window is larger to hide latency.
uint32_t windowFor(size_t nProcesses) {
  constexpr uint32_t budget = 3200; // messages in flight at the receiver; measured: 3200 > 1600 at P = 30, 50
  uint32_t w = budget / static_cast<uint32_t>(std::max<size_t>(nProcesses - 1, 1)) / 32 * 32;
  return std::min(std::max(w, 32u), kMaxWindow);
}

using Clock = std::chrono::steady_clock;

std::atomic<bool> stopFlag{false};

void put32(uint8_t *p, uint32_t v) { v = htonl(v); std::memcpy(p, &v, 4); }
uint32_t get32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return ntohl(v); }
void put16(uint8_t *p, uint16_t v) { v = htons(v); std::memcpy(p, &v, 2); }
uint16_t get16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return ntohs(v); }

// Waits for the socket to become readable; false on timeout.
bool waitReadable(int fd, int timeoutMs) {
  pollfd pfd{fd, POLLIN, 0};
  return poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN);
}

// Sends the given seqs (at most kMaxBatch) as one packet; the payload is the seq itself, treated as opaque bytes.
void sendData(int fd, const sockaddr_in &dst, uint8_t myId, const uint32_t *seqs, unsigned k) {
  uint8_t pkt[3 + kMaxBatch * 10];
  pkt[0] = kData;
  pkt[1] = myId;
  pkt[2] = static_cast<uint8_t>(k);
  uint8_t *p = pkt + 3;
  for (unsigned i = 0; i < k; i++, p += 10) {
    put32(p, seqs[i]);
    put16(p + 4, 4);
    put32(p + 6, seqs[i]);
  }
  sendto(fd, pkt, static_cast<size_t>(p - pkt), 0, reinterpret_cast<const sockaddr *>(&dst), sizeof dst);
}

void runSender(int fd, const sockaddr_in &dst, uint8_t myId, uint32_t m, Logger &log) {
  std::vector<uint8_t> acked(gWindow, 0);
  std::vector<Clock::time_point> sentAt(gWindow);
  uint32_t base = 1, next = 1; // [base, next) in flight, seq 1..m
  auto lastScan = Clock::now();
  uint8_t buf[kMaxPacket];

  while (!stopFlag) {
    auto now = Clock::now();
    while (!stopFlag && next <= m && next - base < gWindow) {
      uint32_t batch[kMaxBatch];
      unsigned k = 0;
      while (k < kMaxBatch && next <= m && next - base < gWindow) {
        log.broadcast(next); // logged before the message first hits the wire
        sentAt[next % gWindow] = now;
        batch[k++] = next++;
      }
      sendData(fd, dst, myId, batch, k);
    }

    if (now - lastScan >= kScanEvery) {
      lastScan = now;
      uint32_t batch[kMaxBatch];
      unsigned k = 0;
      for (uint32_t s = base; s != next && !stopFlag; s++) {
        if (!acked[s % gWindow] && now - sentAt[s % gWindow] >= kRto) {
          sentAt[s % gWindow] = now;
          batch[k++] = s;
          if (k == kMaxBatch) { sendData(fd, dst, myId, batch, k); k = 0; }
        }
      }
      if (k && !stopFlag) sendData(fd, dst, myId, batch, k);
    }

    bool canSend = next <= m && next - base < gWindow;
    if (!canSend && !waitReadable(fd, static_cast<int>(kScanEvery.count()))) continue;
    for (;;) {
      ssize_t r = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (r < 0) break;
      if (stopFlag) return;
      size_t n = static_cast<size_t>(r);
      if (n != ackSize() || buf[0] != kAck) continue;
      uint32_t low = get32(buf + 2);
      if (low > next) continue; // cannot ack what was never sent
      while (base < low) acked[base++ % gWindow] = 0;
      for (unsigned w = 0; w < gWindow / 32; w++) {
        uint32_t mask = get32(buf + 6 + 4 * w);
        for (unsigned i = 0; mask; i++, mask >>= 1) {
          uint32_t seq = low + 1 + 32 * w + i;
          if ((mask & 1) && seq >= base && seq < next) acked[seq % gWindow] = 1;
        }
      }
      while (base != next && acked[base % gWindow]) acked[base++ % gWindow] = 0;
    }
  }
}

struct RxState {
  uint32_t low = 1; // every seq < low has been delivered
  std::bitset<kMaxWindow> got; // delivered seqs in [low, low + gWindow), indexed seq % gWindow
};

// recvmmsg/sendmmsg move up to kBurst packets per syscall; our own DATA packets are at most 3 + 8 x 10 bytes.
constexpr unsigned kBurst = 64;
constexpr size_t kSlot = 256;

void runReceiver(int fd, const std::vector<sockaddr_in> &addrs, uint8_t myId, Logger &log) {
  std::vector<RxState> rx(addrs.size());
  std::vector<uint8_t> touched(addrs.size(), 0);
  static uint8_t in[kBurst][kSlot];
  static uint8_t out[kBurst][kMaxAckSize];
  iovec inIov[kBurst], outIov[kBurst];
  mmsghdr inMsg[kBurst], outMsg[kBurst];
  uint8_t touchedList[kBurst];

  while (!stopFlag) {
    if (!waitReadable(fd, 100)) continue;
    for (;;) {
      for (unsigned i = 0; i < kBurst; i++) {
        inIov[i] = {in[i], kSlot};
        inMsg[i] = {};
        inMsg[i].msg_hdr.msg_iov = &inIov[i];
        inMsg[i].msg_hdr.msg_iovlen = 1;
      }
      int got = recvmmsg(fd, inMsg, kBurst, MSG_DONTWAIT, nullptr);
      if (got <= 0) break;
      if (stopFlag) return;

      unsigned nTouched = 0;
      for (int p = 0; p < got; p++) {
        const uint8_t *buf = in[p];
        size_t n = inMsg[p].msg_len;
        if (n < 3 || buf[0] != kData) continue;
        size_t sender = buf[1];
        if (sender == 0 || sender >= addrs.size()) continue;
        RxState &st = rx[sender];
        size_t off = 3;
        for (unsigned i = 0; i < buf[2] && i < kMaxBatch; i++) {
          if (n - off < 6) break;
          uint32_t seq = get32(buf + off);
          size_t len = get16(buf + off + 4);
          if (n - off - 6 < len) break;
          off += 6 + len;

          if (seq >= st.low + gWindow) continue; // beyond sender's window: drop, it will resend
          if (seq >= st.low && !st.got[seq % gWindow]) {
            st.got[seq % gWindow] = true;
            log.deliver(sender, seq);
            while (st.got[st.low % gWindow]) st.got[st.low++ % gWindow] = false;
          }
        }
        if (!touched[sender]) { // every packet is answered, duplicates included
          touched[sender] = 1;
          touchedList[nTouched++] = static_cast<uint8_t>(sender);
        }
      }
      if (stopFlag) return;

      for (unsigned t = 0; t < nTouched; t++) {
        size_t sender = touchedList[t];
        touched[sender] = 0;
        const RxState &st = rx[sender];
        uint32_t mask[kMaxWindow / 32] = {};
        for (unsigned i = 0; i + 1 < gWindow; i++)
          if (st.got[(st.low + 1 + i) % gWindow]) mask[i / 32] |= uint32_t{1} << (i % 32);
        uint8_t *ack = out[t];
        ack[0] = kAck;
        ack[1] = myId;
        put32(ack + 2, st.low);
        for (unsigned w = 0; w < gWindow / 32; w++) put32(ack + 6 + 4 * w, mask[w]);
        outIov[t] = {ack, ackSize()};
        outMsg[t] = {};
        outMsg[t].msg_hdr.msg_name = const_cast<sockaddr_in *>(&addrs[sender]);
        outMsg[t].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        outMsg[t].msg_hdr.msg_iov = &outIov[t];
        outMsg[t].msg_hdr.msg_iovlen = 1;
      }
      // best effort: unsent acks are recovered by the sender's retransmission
      if (nTouched) sendmmsg(fd, outMsg, nTouched, 0);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr); // inherited by the worker thread

  Parser parser(argc, argv);
  parser.parse();

  unsigned long m = 0, receiver = 0;
  std::ifstream cfg(parser.configPath());
  if (!(cfg >> m >> receiver) || m == 0 || m > UINT32_MAX / 2) return 1;

  size_t maxId = 0;
  auto hosts = parser.hosts();
  for (auto &h : hosts) maxId = std::max(maxId, h.id);
  std::vector<sockaddr_in> addrs(maxId + 1);
  for (auto &h : hosts) {
    addrs[h.id].sin_family = AF_INET;
    addrs[h.id].sin_addr.s_addr = h.ip;
    addrs[h.id].sin_port = h.port;
  }
  if (receiver == 0 || receiver > maxId) return 1;

  gWindow = windowFor(hosts.size());
  size_t myId = parser.id();
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = addrs[myId].sin_port;
  if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&me), sizeof me) < 0) return 1;

  Logger log(parser.outputPath());
  std::thread worker([&] {
    if (myId == receiver)
      runReceiver(fd, addrs, static_cast<uint8_t>(myId), log);
    else
      runSender(fd, addrs[receiver], static_cast<uint8_t>(myId), static_cast<uint32_t>(m), log);
  });

  int sig;
  sigwait(&sigs, &sig);
  stopFlag = true; // worker checks it after every recv and before every send
  worker.join();
  log.flush();
  return 0;
}
