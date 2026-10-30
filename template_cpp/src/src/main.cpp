// ================ INITIAL NECESARY EXPLANATION: ================

// 1. Types of packetss:
//
//    DATA: sender -> receiver, carries 1 to 8 messages.
//    ACK: receiver -> sender, acknowledges the received messags.

// 2. Format of each packet
//
// These were optimsed in order to fit all the informtion in as few data as possible.
// A (big) inspiration was taken from the following reference(s):
// - DATA packet: https://www.rfc-editor.org/info/rfc9260/#section-3.3.1
//      Note: Each chunk carries a seq number, length and the payload.
// - ACK packet (1): https://www.rfc-editor.org/info/rfc9293/#section-3.4
//      Note: TCP's cunulative acknowledgement number X.
// - ACK packet (2): https://www.rfc-editor.org/info/rfc4585/#section-6.2.1
//      Note: Even though the above is used to indicate LOST packets, the idea for the bit indexing/mask is the same.
// All multi-byte integers are in network byte order, using put4/get4/put2/get2 as implemented below.

// 2.1. DATA packet:
//
//      byte 0      byte 1     byte 2           then k messages of 10B each:
//    +--------+------------+---------+   +-----------+-----------+---------------+
//    | type=1 | sender id  |    k    |   | seq (4B)  | len (2B)  | payload (4B)  | x k
//    +--------+------------+---------+   +-----------+-----------+---------------+
//
//    - type:      1 = DATA (kData below).
//    - sender id: the --id of the sending process (1..128, so 1B is enough).
//    - k:         How many messages are in this packet, 1 to 8 (project allows at most 8 messages per packet).
//                 Packing several messages into one packet is much faster than one packet per message.
//    - seq:       the sequence number of the message (1..m).
//    - len:       the length in bytes of the payload that follows (even though it's always 4B for this delivery, the
//                 current implementation includes it, so that I respect the "payload-agnostic" requirement).
//    - payload:   the message content.
//
//    Example: sender 3 sends seqs 5,6 in one packet -> [1][3][2] [5][4][5] [6][4][6] : 1B+1B+1B+(4B+2B+4B)x2 = 23B

// 2.2. ACK packet:
//
//      byte 0      byte 1       bytes 2-5     then W/32 words of 32 bits:
//    +--------+-------------+------------+   +----------------------+
//    | type=2 | receiver id |  low (4B)  |   |  mask word (4B=32b)  | x (W/32)
//    +--------+-------------+------------+   +----------------------+
//
//    - type:      2 = ACK (kAck below).
//    - receiv id: the --id of the receiving process (1..128, so 1B is enough).
//    - low:       Number that acknowledges that all messages with seq < low from that sender were received.
//                 4B = 32bit, since it is assumed that n_messages <= 2^{31}
//                 Example: low=6 for send_id=2 means that we acknowledge that receiver has delivered seq=1,2,3,4,5 from sender 2.
//    - mask:      bit i (counted over all words: word i/32, bit i%32) is set  <=>  "I ALSO have seq low+1+i".
//                 It reports messages that arrived out of order, beyond the first missing one.
//                 It starts at low+1, not low, because seq "low" is by definition missing (if it had arrived, low --> low+1).
//
//    Example (W = 32): the receiver has delivered 1,2,3,4,7,9 from sender 3.
//                      1. Everything < 5 is there -> low = 5.
//                      2. Messages with 7 and 9 were also received.
//                         Rule: seq = low+1+i
//                         2.1.   7  =  5 +1+i -> bit i=1.
//                         2.2.   9  =  5 +1+i -> bit i=3.
//                         mask = 00...001010 (binary) = 10 (decimal).
//             ACK bytes: [2][1][5][10] : 1B+1B+4B+(4B)x1 = 10B.
//
//    Note: this is very helpful, since ACK always describes the receiver's COMPLETE state for that sender (not just "I got packet X"),
//          one ACK covers all earlier ones. Hence, losing an ACK therefore costs nothing: the next ACK says the same and more.
//

// 3. Sliding Window
//
// To reduce memory, a sender may have at most W messages "in flight" (i.e. sent but not yet acknowledged by receiver).
// Only when the oldest ones are acked, it can send new ones: the window=[base,next) "slides" forward. This keeps memory small on both sides
// In this way, the sender never has more than window_size (W) unacknowledged messages, so the receiver only ever needs to track
// the range [low, low + W) for each sender: anything below low is known to be delivered, and nothing at or above low + W can have been sent yet.


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

namespace {

constexpr uint8_t kData = 1, kAck = 2;

// I saw that 256 in-flight messages per sender is already enough to hide latency, and bigger windows gave no gain in measurements.
constexpr uint32_t kMaxWindow = 256;
constexpr uint32_t kMinWindow = 32;

//  window_size starts at 32 and is overwritten in main() given the number of processes involved (windowFor() method)
uint32_t window_size = 32;

// RTO = Retransmission TimeOut: an unacked message is sent again once 500 ms have passed since its last send.
constexpr auto kRTO = std::chrono::milliseconds(500);

// How often the sender scans its window looking for messages whose RTO has expired.
// Also used as the poll() timeout of the sender, so a sender wakes up at least every 50 ms even if nothing arrives.
constexpr auto kScanEvery = std::chrono::milliseconds(50);

// Size in bytes of the largest possible ACK: 6 header bytes (type, id, low) + 4B per 32-bit mask word.
constexpr size_t kMaxAckSize = 6 + 4 * (kMaxWindow / 32);

// Size of the sender's receive buffer, larger than any possible [], so a packet is never truncated (largest valid ACK + 1 byte).
constexpr size_t kMaxPacket = kMaxAckSize + 1;

// Maximum number of messages in one DATA packet (project rule: at most 8 messages per packet).
constexpr unsigned kMaxBatch = 8;


// Helper function that computes size in bytes of an ACK for the CURRENT window: 6 header bytes + 4B per mask word (W/32 words).
size_t ackSize() { 
  return 6 + 4 * size_t{window_size / 32};
}

// Concerning the window_size, the idea is that for few senders, throughput is limited by latency (so each sender should get
// a big window) and for many senders, the bottleneck becomes the receiver's socket buffer, which overflows if too many packets
// arrive at once (hence small window is prefered). 
//
// Thus, window_size ~ A/nProcesses
// A was chosen to be = 3200 after some tests (gave more throughput than 1600, and 6400 or more gave no significant gain).
uint32_t windowFor(size_t nProcesses) {
  constexpr uint32_t A = 3200;
  uint32_t nprocs;
  if (nProcesses < 1) {
    nprocs = 1;
  } else {
    nprocs = static_cast<uint32_t>(nProcesses);
  }
  // window_Size should be a multiple of 32, since ACK mask is sent as whole 32-bit words (W bits are split into W/32 words).
  uint32_t w = (A / nprocs)/32 * 32; // Round down to nearest multiple of 32.
  if (w > kMaxWindow) {
    return kMaxWindow; // Clamp above by 256.
  } else if (w < kMinWindow) {
    return kMinWindow; // Clamp below by 32.
  }
  return w; // Finally, if 32 < w < 256, return the computed window.
}

// Set a steady_clock, the recommended option to measure timeouts.
// Ref: https://en.cppreference.com/cpp/chrono/steady_clock ("steady_clock [...] is most suitable for measuring intervals.")
using Clock = std::chrono::steady_clock;

// Set to true by the main thread when there is a SIGTERM/SIGINT. Worker thread checks it after every receive and
// before every send, so it stops all network activity right away. std::atomic makes this safe.
std::atomic<bool> stopFlag{false};


// These helper functions are used to fill the contents of the DATA & ACK packets.
// THey write info v in byte position p, which can be either 2B or 4B (hence put/get have 2B and 4B versions).
// htonl/htons = "host to network long/short", ntohl/ntohs = reverse.
// memcpy used to handle p safely, since it might not be aligned to 4B.
void put4(uint8_t *p, uint32_t v) { 
  v = htonl(v); 
  std::memcpy(p, &v, 4); 
}
uint32_t get4(const uint8_t *p) {
  uint32_t v; 
  std::memcpy(&v, p, 4);
  return ntohl(v);
}
void put2(uint8_t *p, uint16_t v) {
  v = htons(v); 
  std::memcpy(p, &v, 2);
}
uint16_t get2(const uint8_t *p) {
  uint16_t v; 
  std::memcpy(&v, p, 2);
  return ntohs(v);
}

// Function that checks if the socket has received any DATA/ACK packet.
// If nothing, it puts the worker thread (the one managing sends/receives) to sleep for a timeout.
// Instead, if a packet is ready, this retuns true. 
// This allows the thread to not consume CPU resources when nothing is ready.
bool waitReadable(int sock, int timeoutMs) {
  pollfd poll_sock{sock, POLLIN, 0};                                  // watch the socket for "data available to read" (POLLIN)
  return poll(&poll_sock, 1, timeoutMs) > 0 && (poll_sock.revents & POLLIN);
}

// Function that builds one DATA packet (containing the k <= kMaxBatch messages whose seqs are in seqs[0..k-1]), and sends it to the socket.
void sendData(int sock, const sockaddr_in &sock_adrr, uint8_t myId, const uint32_t *seqs, uint8_t k) {
  uint8_t pkt[3 + kMaxBatch * 10];  // 3 header bytes (below) + up to 8 entries of 10B each (4B for seq + 2B for len + 4B for payload) = 83B
  pkt[0] = kData;         // byte 0: packet type
  pkt[1] = myId;          // byte 1: who is sending the msg
  pkt[2] = k;             // byte 2: how many messages follow
  uint8_t *p = pkt + 3;   // p = where the next entry will be written

  for (unsigned i = 0; i < k; i++) { // Words of 10B:
    put4(p, seqs[i]);     // seq, which is 4B (hence written with put4) in the first section of the word (hence in position p), writing the i-th seq to send (hence seqs[i])
    put2(p + 4, 4);       // len, which is 2B (hence written with put2) written after 'seq'=4B (hence in position p+4), writing the payload length = 4B (hence the 4)
    put4(p + 6, seqs[i]); // payload, which is 4B (hence written with put4) written after 'len'=2B (hence in position p+4+2), writing the message content = i-th seq (hence also seqs[i])
    p += 10;
  }
  size_t pkt_len = 3 + 10*size_t{k};
  sendto(sock, pkt, pkt_len, 0, reinterpret_cast<const sockaddr *>(&sock_adrr), sizeof sock_adrr);
}

// SENDER
// Runs in the worker thread of every process that is NOT the receiver. Sends messages 1..m to sock_adrr (the receiver),

//  State:
//    base = the oldest seq that is not yet acknowledged. Every seq < base is acked.
//    next = the next seq that has never been sent. Seqs in [base, next) are "in flight".

//  Each loop iteration does 3 steps:
//    STEP 1: send new messages while the window has room (in packets of up to 8 messages).
//    STEP 2: every 50 ms, resend every in-flight message unacked for >= 500 ms.
//    STEP 3: read all the ACKs waiting on the socket and update base/acked (this is what slides the window).
// =====================================================================================================================
void runSender(int sock, const sockaddr_in &sock_adrr, uint8_t myId, uint32_t m, Logger &log) {
  std::vector<uint8_t> acked(window_size, 0);            // Vector where acked[i]=1 means that message in i-th position of window is acknowledged. 0 indicates that all positions (window msgs) start initially non-acknowledged.
  std::vector<Clock::time_point> sentAt(window_size);    // Vector where sentAt[i]=27ms means that msg in i-th position of window was last sent 27ms ago.
  uint32_t base = 1, next = 1;                            // Lower and higher ends of the window: [base, next) containing the in-flight msgs (those sent but not yet acked).
  auto lastScan = Clock::now();                           // Initialise variable that will check when STEP 2 last ran.
  uint8_t buf[kMaxPacket];                                // Receive buffer for incoming ACK packets

  while (!stopFlag) { // As long as there's not any SIGINT/SIGTERM (all processes are running nominally),
    auto now = Clock::now(); // read the clock (done once per iteration)

    // ---- STEP 1: SEND NEW DATA MSGS ----
    while (!stopFlag && next <= m && next - base < window_size) { // While still msgs to be sent AND while they can be sent (i.e. there is still size in the window),
      uint32_t batch[kMaxBatch]; // batch of msgs to be sent within this DATA packet (up to 8 = kMaxBatch)
      unsigned k = 0;            // how many seqs are included in the packet so far
      while (k < kMaxBatch && next <= m && next - base < window_size) { // Same conditions, but checked for every msg in the packet, since these conditions can change for the LAST packet (m might not be a multiple of kMaxBatch=8).
        log.broadcast(next);          // Log "b next" BEFORE it is sent for the first time (to satisfy PL3, since we could be killed between sendto() and the logging, where PL3 would be broken).
        sentAt[next % window_size] = now; // start its transmission timer
        batch[k++] = next++;          // add it to the packet; it is now in flight
      }
      sendData(sock, sock_adrr, myId, batch, k); // Send the whole packet with up to 8 msgs.
    }

    // ---- STEP 2: RETRANSMIT EXPIRED MSGS  -----
    if (now - lastScan >= kScanEvery) { // This is checked at most once every kScanEvery = 50 ms.
      lastScan = now;
      uint32_t batch[kMaxBatch];
      unsigned k = 0;
      for (uint32_t s = base; s < next && !stopFlag; s++) { // Walk through every in-flight seq [base, next).
        if (!acked[s % window_size] && now - sentAt[s % window_size] >= kRTO) { // If not acked and was last sent >= 500ms ago,
          sentAt[s % window_size] = now;
          batch[k++] = s; // include it in the batck of msgs to be sent again.
          if (k == kMaxBatch) { // Once batch is full (8 msgs)
            sendData(sock, sock_adrr, myId, batch, k); // send it, and start a new one.
            k = 0; 
          }
        }
      }
      if ((k != 0) && (!stopFlag)) { // If we scan everything that is in flight, but this only partially fills a packet (k>0):
        sendData(sock, sock_adrr, myId, batch, k); // send it as well.
      }
    }

    // Check if an ACK arrived. If so, this reads false and we go to step 3, where this/these ACKs are considered.
    // Otherwise, if no ACK packets received, if performs a timeout of 50ms to not waste CPU resources, and we skip STEP 3 and go back to STEP 1 transmission or STEP 2 retransmit check.
    if (!waitReadable(sock, static_cast<int>(kScanEvery.count()))) {
      continue;
    }

    // ---- STEP 3: READ ALL ACKs WAITING ON SOCKET ----
    // If we are here, then there are some ACKs waiting! Let's process them.
    while (true) {
      ssize_t r = recv(sock, buf, sizeof buf, MSG_DONTWAIT); // MSG_DONTWAIT: flag such that, when no ACK packet is left, recv returns -1 and we leave STEP 3.
      if (r < 0) { // no more ACK packets waiting
        break; // Back to STEP 1.
      }
      if (stopFlag) { // check for potential SIGINT/SIGTERM 
        return; // Exit run.
      }
      size_t n = static_cast<size_t>(r); // packet length in bytes
      if (n != ackSize() || buf[0] != kAck) { // Just a check, to ignore potentially broken packets.
        continue; // go to next ACK packet.
      }
      uint32_t low = get4(buf + 2); // Since the 'low' value is 4B (hence get4), and sits 2B after beginning of ACK packet (hence buf+2): [0: packetType][1: myId][2: low][...]

      if (low > next) { // quick check, since next is the upper msg in the window (msgs sent not acked). Hence, everything larger was never sent!
        continue; // go to next ACK packet.
      }
      // 'low' tells us that all msgs with seq < low have been delivered
      while (base < low) {
        acked[base % window_size] = 0; // reset acked to 0, since those acked positions will now be used for new msgs (base+W)
        base++; // slide base of the window upwards.
      }
      // 'mask' tells us those messages with seq > low that have also been delivered
      // max_window = 256, and every msg is encoded with 1 bit, so there is no integer type that wide.
      // Hence we chunk the mask into 32-bit "words", s.t. bit i of word w means that seq = (low + 1) + (32*w + i) has been delivered.
      for (unsigned w = 0; w < window_size/32; w++) { // For every word index (0, 1,...)
        uint32_t bits = get4(buf + 6 + 4*w); // get the 4B word corresponding to that index (sitting after 6B header + 4B per word).
        uint32_t seq = low + 1 + 32*w; // the seq of the lowest bit
        while (bits != 0) {
          bool arrived = bits & 1;                       // look at the lowest bit of the word (marked as "1" if it was delivered) hence this results true it the msg whose seq matches that bit was received
          if (arrived && seq >= base && seq < next) {    // if so,
            acked[seq % window_size] = 1;                // mark is as acked.
          }
          bits >>= 1;                                    // go to the next bit in the word (which becomes the lowest)
          seq++;                                         // whose seq value stands for one more
        }
      }
      // Now, from those msgs in the window, take those that were acked and set their ack value to 0, since those positions will be now used for the next msgs.
      while (base < next && acked[base % window_size]) { // start from the lower one, and see if it was acked
        acked[base % window_size] = 0; // de-ack it, 
        base++; // indicating the next message
      }
    }
  }
}

// Receiver state:
struct RxState {
  uint32_t low = 1; // low : every seq < low from this sender has been delivered (so any seq < low that arrives is a duplicate).
  std::bitset<kMaxWindow> got; // got : which seqs in [low, low + W) have been delivered, one bit each, stored at index seq % W.
};

// We use recvmmsg/sendmmsg to send multiple ACK packets. They will send up to "kBurst" packets per syscall.
constexpr unsigned kBurst = 64;
constexpr size_t kSlot = 3 + kMaxBatch*10 + 1; // max allocated size (in bytes) per DATA packet (these are at most 3B+8Bx10 = 83B, so 83+1 does the job).


// RECEIVER
// Runs in the worker thread of the receiver process. 
// Loops until stopFlag becomes true, where every iteration = BURST does these 4 steps:
//    1. Sleep until packets arrive (poll with a 100 ms timeout, so stopFlag is checked at least every 100 ms).
//    2. Read up to 64 packets at once (recvmmsg).
//    3. For every message in every DATA packet: if it is new, log "d sender seq" (deliver it) and remember it.
//       Duplicates are ignored, and messages are delivered as soon as they arrive.
//    4. Send one ACK to each sender that appeared in this burst (all ACKs in a single sendmmsg call).

void runReceiver(int sock, const std::vector<sockaddr_in> &addrs, uint8_t myId, Logger &log) {
  std::vector<RxState> rx(addrs.size());           // vector where rx[sender_id] stores the delivery state for that sender
  std::vector<uint8_t> senders_to_ack(addrs.size(), 0);   // vector where senders_to_ack[id]=1 means that sender 'id' sent a packet in the current burst
  
  // Receive and send buffers: "static" puts them in global memory so they are created once instead of on every thread call.
  static uint8_t in[kBurst][kSlot];                // in[i] = bytes of the i-th DATA packet of the burst
  static uint8_t out[kBurst][kMaxAckSize];         // out[j] = bytes of the j-th ACK we are about to send
  
  // Structures that the recvmmsg/sendmmsg calls will need.
  iovec inIov[kBurst], outIov[kBurst];
  mmsghdr inMsg[kBurst], outMsg[kBurst];

  // The list of senders for each recv DATA packet in this burst. 
  // Each has at most 64 packets, so an array of 64 senders will cover all cases.
  uint8_t senders_list[kBurst];

  while (!stopFlag) {
    // ---- STEP 1: Sleep if no packets have arrived ----
    if (!waitReadable(sock, 100)) {
      continue; // sleep for 100ms (to not waste CPU resources).
    }
    while (true) {
      // ---- STEP 2: Read up to 64 packets at once ----
      // Prepare the 64 receive slots
      for (unsigned i = 0; i < kBurst; i++) {
        inIov[i] = {in[i], kSlot};
        inMsg[i] = {};
        inMsg[i].msg_hdr.msg_iov = &inIov[i];
        inMsg[i].msg_hdr.msg_iovlen = 1;
      }
      // Receive up to 64 packets in one system call.
      int got = recvmmsg(sock, inMsg, kBurst, MSG_DONTWAIT, nullptr); // how many packets were read.
      if (got <= 0) {
        break; // socket empty: go back to beginning of while, and sleep (waitReadable).
      }
      if (stopFlag) { // check for potential SIGINT/SIGTERM 
        return;
      }

      unsigned n_senders = 0; // how many senders are there in this burst
      for (int p = 0; p < got; p++) { // for each DATA packet received,
        const uint8_t *buf = in[p]; // pointer to first byte of the packet
        size_t n = inMsg[p].msg_len; // the packet's length in bytes

        if (n < 3 || buf[0] != kData) { // Just a check, to ignore potentially broken packets (packet type and lengthdon't match)
          continue;
        }
        size_t sender = buf[1];
        if (sender == 0 || sender >= addrs.size()) { // not a valid process id from the HOSTS file
          continue;
        }
        RxState &st = rx[sender]; // get the state of the sender of that packet.
        size_t off = 3; // read position: first message entry starts after the 3B header
        
        // ---- STEP 3: Process the messages ----
        // buf[0]: packet type, buf[1]: sender id, buf[2]: beginning of message.
        for (unsigned i = 0; i < buf[2] && i < kMaxBatch; i++) {
          if (n - off < 6) { // quick check to see if the header of the next message is there (6B long)
            break;
          }
          uint32_t seq = get4(buf + off); // seq is 4B value (hence obtained with get4) at 'off' position (hence buff+off)
          size_t len = get2(buf + off + 4); // len is 2B value (hence obtained wth get2) 4B after the 'seq' (hence the +4)
          if (n - off - 6 < len) {
            break;
          }
          off += 6 + len; // move the 'off' position

          // What do we do with the msg?
          if (seq >= st.low + window_size) { // seq outside the window, so it can't have been sent yet
            continue; // drop it
          }
          if (seq >= st.low && !st.got[seq % window_size]) { // if within the window
            st.got[seq % window_size] = true; // remember it, to avoid duplicates
            log.deliver(sender, seq); // DELIVER: writes "d sender seq" to output
            // Slide window 'low' forward over every consecutive delivered seq, clearing each bit as we pass it
            while (st.got[st.low % window_size]) { // if msg already delivered
              st.got[st.low % window_size] = false; // clear it false (for the next one to arrive, which will cover its position)
              st.low++; // move to the next seq.
            }
          }
        }
        // Send ACKs, even in case of duplicate, since ACK packets could have dropped
        if (!senders_to_ack[sender]) { // every packet is answered, duplicates included
          senders_to_ack[sender] = 1; // acknowledge it
          senders_list[n_senders] = static_cast<uint8_t>(sender); // add it in the list of senders
          n_senders++;
        }
      }
      if (stopFlag) { // check for potential SIGINT/SIGTERM
        return;
      }

      // ---- STEP 4: Prepare the ACK burst to be sent ----

      // Build one ACK packet *per sender*, describing the full state for that sender (low + mask).
      for (unsigned t = 0; t < n_senders; t++) {
        size_t sender = senders_list[t];
        senders_to_ack[sender] = 0; // reset for the next burst
        const RxState &st = rx[sender];
        // Build the mask: bit i <=> seq low + 1 + i has been delivered, for i = 0 .. W-2 (seqs low+1 .. low+W-1).
        uint32_t mask[kMaxWindow / 32] = {};
        for (unsigned i = 0; i < window_size-1; i++) {
          if (st.got[(st.low + 1 + i) % window_size]) { // have we received such seq msg?
            mask[i / 32] |= uint32_t{1} << (i % 32); // turn on bit i of the whole mask: bit i%32 of word i/32
          }
        }
        // Write the ACK packet: [type][id][low][mask words...]
        uint8_t *ack = out[t]; // take sender 't'
        ack[0] = kAck;
        ack[1] = myId;
        put4(ack + 2, st.low); // 'low' is 4B (hence put4) and goes in 2nd position (hence ack+2)

        // Write the mask of received messages for every 32bit = 4B word.
        for (unsigned w = 0; w < window_size/32; w++) {
          put4(ack + 6 + 4*w, mask[w]);
        }
        // Fill in the sendmmsg entry: which bytes to send (outIov) and to whom (msg_name = the sender's address).
        outIov[t] = {ack, ackSize()};
        outMsg[t] = {};
        outMsg[t].msg_hdr.msg_name = const_cast<sockaddr_in *>(&addrs[sender]); // since this call reqs a non-const pointer.
        outMsg[t].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        outMsg[t].msg_hdr.msg_iov = &outIov[t];
        outMsg[t].msg_hdr.msg_iovlen = 1;
      }
      
      if (n_senders) { // This checks if there are some to-be-acknowledged senders.
        sendmmsg(sock, outMsg, n_senders, 0); // Send all the ACKs in ONE system call.
      }
    }
  }
}

}

// MAIN
int main(int argc, char **argv) {
  // ---- 1. Signals ----
  // A signal handler could interrupt a thread at any point (e.g. during sendto() or log).
  // I chose to BLOCK SIGINT/SIGTERM before creating the worker (which inherits the mask), so that no thread is ever interrupted:
  //  - main later collects the signal with sigwait() (as a normal return)
  //  - then sets stopFlag (worker stops all network activity), joins the worker, and writes the log.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr); // inherited by the worker thread


  // ---- 2. Arguments, config, HOSTS ----
  Parser parser(argc, argv);
  parser.parse();

  // Read number of messages and receiver process.
  unsigned long m = 0, receiver = 0;
  std::ifstream cfg(parser.configPath());
  if (!(cfg >> m >> receiver) || m == 0 || m > UINT32_MAX / 2) {
    return 1;
  }

  // Build address book, where addrs[id] = the IPv4 address + port of process id, for fast lookup by id.
  // Get max process id.
  size_t maxId = 0;
  auto hosts = parser.hosts();
  for (auto &h : hosts) {
    maxId = std::max(maxId, h.id);
  }
  std::vector<sockaddr_in> addrs(maxId + 1); // build the vector
  // and store each information for each host.
  for (auto &h : hosts) {
    addrs[h.id].sin_family = AF_INET;
    addrs[h.id].sin_addr.s_addr = h.ip;
    addrs[h.id].sin_port = h.port;
  }
  // Quick check:
  if (receiver == 0 || receiver > maxId) {
    return 1; // the receiver id must exist
  }

  // ---- 3. Window size (discussed in the implementation above) ----
  window_size = windowFor(hosts.size());
  size_t myId = parser.id();

  // ---- 4. The UDP socket ----
  // Create the single UDP socket and bind it to our own port (from the HOSTS file).
  int sock = socket(AF_INET, SOCK_DGRAM, 0); // SOCK_DGRAM = UDP
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY); // INADDR_ANY = accept packets arriving on any of this machine's network interfaces.
  me.sin_port = addrs[myId].sin_port;
  if (sock < 0 || bind(sock, reinterpret_cast<sockaddr *>(&me), sizeof me) < 0) {
    return 1;
  }

  // ---- 5. Output file + start worker thread ----
  Logger log(parser.outputPath());
  std::thread worker([&] { // [&]: worker captures everything by reference
    if (myId == receiver)
      runReceiver(sock, addrs, static_cast<uint8_t>(myId), log);
    else
      runSender(sock, addrs[receiver], static_cast<uint8_t>(myId), static_cast<uint32_t>(m), log);
  });

  // ---- 6. Sleep until in sigwait() until SIGTERM/SIGINT arrive ----
  int sig;
  sigwait(&sigs, &sig);

  // ---- 7. Shutdown ----
  stopFlag = true; // set stopFlag -> worker checks it after every recv and before every send, so it will stop all activity in <=100ms and return.
  worker.join();
  log.flush(); // write the remaining buffered logs.
  return 0;
}
