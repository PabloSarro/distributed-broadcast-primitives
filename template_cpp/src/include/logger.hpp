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
    buf_.reserve(kFlushAt + 64);
  }
  ~Logger() {
    if (fd_ >= 0) close(fd_);
  }

  void broadcast(uint32_t seq) {
    buf_ += "b ";
    buf_ += std::to_string(seq);
    buf_ += '\n';
    maybeFlush();
  }

  void deliver(size_t sender, uint32_t seq) {
    buf_ += "d ";
    buf_ += std::to_string(sender);
    buf_ += ' ';
    buf_ += std::to_string(seq);
    buf_ += '\n';
    maybeFlush();
  }

  void flush() {
    size_t off = 0;
    while (fd_ >= 0 && off < buf_.size()) {
      ssize_t n = write(fd_, buf_.data() + off, buf_.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
    buf_.clear();
  }

private:
  static constexpr size_t kFlushAt = 1 << 16;
  void maybeFlush() {
    if (buf_.size() >= kFlushAt) flush();
  }

  int fd_;
  std::string buf_;
};
