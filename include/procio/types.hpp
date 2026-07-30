#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace procio {

// ── Process I/O statistics ──
struct ProcessIO {
  int pid = 0;
  std::string comm;

  // From /proc/[pid]/io
  unsigned long long rchar = 0;          // bytes read (syscall-level)
  unsigned long long wchar = 0;          // bytes written (syscall-level)
  unsigned long long read_bytes = 0;     // bytes read (actual storage I/O)
  unsigned long long write_bytes = 0;    // bytes written (actual storage I/O)
  unsigned long long cancelled_write_bytes = 0;

  [[nodiscard]] unsigned long long total_syscall() const noexcept {
    return rchar + wchar;
  }

  [[nodiscard]] unsigned long long total_storage() const noexcept {
    return read_bytes + write_bytes;
  }
};

// ── Snapshot of all processes ──
struct IOSnapshot {
  std::vector<ProcessIO> processes;
  unsigned long long total_rchar = 0;
  unsigned long long total_wchar = 0;
  unsigned long long total_read_bytes = 0;
  unsigned long long total_write_bytes = 0;

  void recompute_totals() {
    total_rchar = 0;
    total_wchar = 0;
    total_read_bytes = 0;
    total_write_bytes = 0;
    for (const auto& p : processes) {
      total_rchar += p.rchar;
      total_wchar += p.wchar;
      total_read_bytes += p.read_bytes;
      total_write_bytes += p.write_bytes;
    }
  }
};

// ── CLI options ──
struct Options {
  bool watch = false;
  int interval = 2;           // seconds between refreshes
  int max_rows = 20;          // max processes to show
  int pid_filter = 0;         // 0 = all processes
  bool json_output = false;
  bool ansi_color = true;
  bool help = false;
  std::string sort_by = "total";  // rchar, wchar, read, write, total
};

} // namespace procio
