#include <arpa/inet.h>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include "logger.hpp"
#include "parser.hpp"

// Wire format (network byte order):
//   DATA: 1 | u8 sender | u8 k | k x (u32 seq | u16 len | payload)
//   ACK:  2 | u8 receiver | u8 k | k x u32 seq   (acks every seq of the DATA packet it answers)
// Sender and receiver share the window size: the sender never has more than
// kWindow unacked seqs, so the receiver can track [low, low+kWindow) in a bitset.
namespace {

constexpr uint8_t kData = 1, kAck = 2;
constexpr uint32_t kWindow = 32;
constexpr auto kRto = std::chrono::milliseconds(500);
constexpr auto kScanEvery = std::chrono::milliseconds(50);
constexpr size_t kMaxPacket = 65536;
constexpr unsigned kMaxBatch = 8; // messages per packet (project limit)

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

void sendAck(int fd, const sockaddr_in &dst, uint8_t myId, const uint32_t *seqs, unsigned k) {
  uint8_t pkt[3 + kMaxBatch * 4];
  pkt[0] = kAck;
  pkt[1] = myId;
  pkt[2] = static_cast<uint8_t>(k);
  for (unsigned i = 0; i < k; i++) put32(pkt + 3 + 4 * i, seqs[i]);
  sendto(fd, pkt, 3 + 4 * k, 0, reinterpret_cast<const sockaddr *>(&dst), sizeof dst);
}

void runSender(int fd, const sockaddr_in &dst, uint8_t myId, uint32_t m, Logger &log) {
  std::vector<uint8_t> acked(kWindow, 0);
  std::vector<Clock::time_point> sentAt(kWindow);
  uint32_t base = 1, next = 1; // [base, next) in flight, seq 1..m
  auto lastScan = Clock::now();
  uint8_t buf[kMaxPacket];

  while (!stopFlag) {
    auto now = Clock::now();
    while (!stopFlag && next <= m && next - base < kWindow) {
      uint32_t batch[kMaxBatch];
      unsigned k = 0;
      while (k < kMaxBatch && next <= m && next - base < kWindow) {
        log.broadcast(next); // logged before the message first hits the wire
        sentAt[next % kWindow] = now;
        batch[k++] = next++;
      }
      sendData(fd, dst, myId, batch, k);
    }

    if (now - lastScan >= kScanEvery) {
      lastScan = now;
      uint32_t batch[kMaxBatch];
      unsigned k = 0;
      for (uint32_t s = base; s != next && !stopFlag; s++) {
        if (!acked[s % kWindow] && now - sentAt[s % kWindow] >= kRto) {
          sentAt[s % kWindow] = now;
          batch[k++] = s;
          if (k == kMaxBatch) { sendData(fd, dst, myId, batch, k); k = 0; }
        }
      }
      if (k && !stopFlag) sendData(fd, dst, myId, batch, k);
    }

    bool canSend = next <= m && next - base < kWindow;
    if (!canSend && !waitReadable(fd, static_cast<int>(kScanEvery.count()))) continue;
    for (;;) {
      ssize_t r = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (r < 0) break;
      if (stopFlag) return;
      size_t n = static_cast<size_t>(r);
      if (n < 3 || buf[0] != kAck || n != 3 + 4 * size_t{buf[2]}) continue;
      for (unsigned i = 0; i < buf[2]; i++) {
        uint32_t seq = get32(buf + 3 + 4 * i);
        if (seq < base || seq >= next) continue;
        acked[seq % kWindow] = 1;
      }
      while (base != next && acked[base % kWindow]) acked[base++ % kWindow] = 0;
    }
  }
}

struct RxState {
  uint32_t low = 1; // every seq < low has been delivered
  std::bitset<kWindow> got; // delivered seqs in [low, low + kWindow)
};

void runReceiver(int fd, const std::vector<sockaddr_in> &addrs, uint8_t myId, Logger &log) {
  std::vector<RxState> rx(addrs.size());
  uint8_t buf[kMaxPacket];

  while (!stopFlag) {
    if (!waitReadable(fd, 100)) continue;
    for (;;) {
      ssize_t r = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
      if (r < 0) break;
      if (stopFlag) return;
      size_t n = static_cast<size_t>(r);
      if (n < 3 || buf[0] != kData) continue;
      size_t sender = buf[1];
      if (sender == 0 || sender >= addrs.size()) continue;
      size_t off = 3;
      uint32_t toAck[kMaxBatch];
      unsigned nAck = 0;
      for (unsigned i = 0; i < buf[2] && i < kMaxBatch; i++) {
        if (n - off < 6) break;
        uint32_t seq = get32(buf + off);
        size_t len = get16(buf + off + 4);
        if (n - off - 6 < len) break;
        off += 6 + len;

        RxState &st = rx[sender];
        if (seq >= st.low + kWindow) continue; // beyond sender's window: drop, it will resend
        if (seq >= st.low && !st.got[seq % kWindow]) {
          st.got[seq % kWindow] = true;
          log.deliver(sender, seq);
          while (st.got[st.low % kWindow]) st.got[st.low++ % kWindow] = false;
        }
        toAck[nAck++] = seq; // always ack, duplicates included
      }
      if (nAck) sendAck(fd, addrs[sender], myId, toAck, nAck);
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
