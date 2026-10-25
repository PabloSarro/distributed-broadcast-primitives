#pragma once

#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// Buffers whole lines in memory; flush() writes them with write(2).
// Not thread-safe: only the worker thread logs, main flushes after joining it.
class Logger {
public:
  explicit Logger(const std::string &path)
      : fd_(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)) {
    buf_.resize(kFlushAt + kMaxLine);
  }
  ~Logger() {
    if (fd_ >= 0) close(fd_);
  }

  void broadcast(uint32_t seq) {
    char *p = tail();
    *p++ = 'b';
    *p++ = ' ';
    p = putUint(p, seq);
    *p++ = '\n';
    commit(p);
  }

  void deliver(size_t sender, uint32_t seq) {
    char *p = tail();
    *p++ = 'd';
    *p++ = ' ';
    p = putUint(p, static_cast<uint32_t>(sender));
    *p++ = ' ';
    p = putUint(p, seq);
    *p++ = '\n';
    commit(p);
  }

  void flush() {
    size_t off = 0;
    while (fd_ >= 0 && off < len_) {
      ssize_t n = write(fd_, buf_.data() + off, len_ - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    len_ = 0;
  }

private:
  static constexpr size_t kFlushAt = 1 << 16;
  static constexpr size_t kMaxLine = 32; // "d " + 3 + " " + 10 digits + "\n" fits easily

  char *tail() { return buf_.data() + len_; }
  void commit(char *end) {
    len_ = static_cast<size_t>(end - buf_.data());
    if (len_ >= kFlushAt) flush();
  }
  static char *putUint(char *p, uint32_t v) {
    char tmp[10];
    int n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
  }

  int fd_;
  std::string buf_;
  size_t len_ = 0;
};
