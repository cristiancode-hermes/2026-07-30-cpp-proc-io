// SPDX-License-Identifier: MIT
// cpp-proc-io — Linux Process I/O Monitor
// Reads /proc/[pid]/io for all processes and displays I/O statistics.
//
// Build:  g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o proc-io
// Usage:  ./proc-io                    # one-shot, top 20 by total I/O
//         ./proc-io --watch            # refresh every 2 seconds
//         ./proc-io -w -n 10           # top 10, refresh every 2s
//         ./proc-io --json             # JSON output (machine-readable)
//         ./proc-io --pid 1234         # single process
//         ./proc-io --sort rchar       # sort by read syscall bytes

#include "procio/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace procio = procio;

// ── Forward declarations ──
std::vector<procio::ProcessIO> read_all_processes(int pid_filter);
procio::ProcessIO read_process_io(int pid);
std::string read_process_comm(int pid);
procio::IOSnapshot collect_snapshot(int pid_filter);
void print_table(const procio::IOSnapshot& snap, const procio::Options& opts);
void print_json(const procio::IOSnapshot& snap);
std::string fmt_bytes(unsigned long long bytes);
std::string fmt_num(unsigned long long n);
void clear_screen();
[[maybe_unused]] static bool parse_args(int argc, char** argv, procio::Options& opts);

// ── Entry point ──
int main(int argc, char** argv) {
  procio::Options opts;
  if (!parse_args(argc, argv, opts)) {
    return 1;
  }

  if (opts.help) {
    std::println(std::cout, "proc-io — Linux Process I/O Monitor");
    std::println(std::cout, "");
    std::println(std::cout, "Usage:  {} [options]", argv[0]);
    std::println(std::cout, "");
    std::println(std::cout, "Options:");
    std::println(std::cout, "  -w, --watch         Refresh every N seconds (default: 2)");
    std::println(std::cout, "  -i, --interval N    Refresh interval in seconds (default: 2)");
    std::println(std::cout, "  -n, --max-rows N    Max processes to show (default: 20)");
    std::println(std::cout, "  -p, --pid N         Monitor a single PID");
    std::println(std::cout, "  -s, --sort FIELD    Sort by: rchar, wchar, read, write, total");
    std::println(std::cout, "  -j, --json          Output as JSON (machine-readable)");
    std::println(std::cout, "  --no-color          Disable ANSI color output");
    std::println(std::cout, "  -h, --help          Show this help");
    return 0;
  }

  if (opts.watch) {
    // Watch mode: continuous refresh
    while (true) {
      auto snap = collect_snapshot(opts.pid_filter);
      clear_screen();
      if (opts.json_output) {
        print_json(snap);
      } else {
        print_table(snap, opts);
      }
      std::this_thread::sleep_for(std::chrono::seconds(opts.interval));
    }
  } else {
    // One-shot mode
    auto snap = collect_snapshot(opts.pid_filter);
    if (opts.json_output) {
      print_json(snap);
    } else {
      print_table(snap, opts);
    }
  }

  return 0;
}

// ── Parse command-line arguments ──
[[maybe_unused]] static bool parse_args(int argc, char** argv, procio::Options& opts) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};

    if (arg == "-h" || arg == "--help") {
      opts.help = true;
    } else if (arg == "-w" || arg == "--watch") {
      opts.watch = true;
    } else if (arg == "-j" || arg == "--json") {
      opts.json_output = true;
    } else if (arg == "--no-color") {
      opts.ansi_color = false;
    } else if ((arg == "-i" || arg == "--interval") && i + 1 < argc) {
      ++i;
      opts.interval = std::stoi(argv[i]);
      if (opts.interval < 1) opts.interval = 1;
    } else if ((arg == "-n" || arg == "--max-rows") && i + 1 < argc) {
      ++i;
      opts.max_rows = std::stoi(argv[i]);
      if (opts.max_rows < 1) opts.max_rows = 1;
    } else if ((arg == "-p" || arg == "--pid") && i + 1 < argc) {
      ++i;
      opts.pid_filter = std::stoi(argv[i]);
      if (opts.pid_filter < 1) {
        std::println(std::cerr, "Error: invalid PID {}", argv[i]);
        return false;
      }
    } else if ((arg == "-s" || arg == "--sort") && i + 1 < argc) {
      ++i;
      opts.sort_by = argv[i];
    } else {
      std::println(std::cerr, "Error: unknown option: {}", arg);
      std::println(std::cerr, "Usage: {} [--help]", argv[0]);
      return false;
    }
  }
  return true;
}

// ── Collect a complete snapshot ──
procio::IOSnapshot collect_snapshot(int pid_filter) {
  procio::IOSnapshot snap;
  snap.processes = read_all_processes(pid_filter);
  snap.recompute_totals();
  return snap;
}

// ── Read I/O for all processes ──
std::vector<procio::ProcessIO> read_all_processes(int pid_filter) {
  std::vector<procio::ProcessIO> result;

  if (pid_filter > 0) {
    auto pio = read_process_io(pid_filter);
    if (pio.pid > 0) {
      result.push_back(std::move(pio));
    }
    return result;
  }

  // Scan /proc for numeric directories
  try {
    for (const auto& entry : fs::directory_iterator("/proc")) {
      if (!entry.is_directory()) continue;

      std::string dirname = entry.path().filename().string();
      int pid = 0;
      auto [ptr, ec] = std::from_chars(dirname.data(), dirname.data() + dirname.size(), pid);
      if (ec != std::errc{} || pid == 0) continue;

      auto pio = read_process_io(pid);
      if (pio.pid > 0) {
        result.push_back(std::move(pio));
      }
    }
  } catch (const fs::filesystem_error& e) {
    // /proc might not be accessible in some containers
    std::println(std::cerr, "Warning: cannot scan /proc: {}", e.what());
  }

  return result;
}

// ── Read /proc/[pid]/io ──
procio::ProcessIO read_process_io(int pid) {
  procio::ProcessIO pio;
  pio.pid = pid;
  pio.comm = read_process_comm(pid);

  std::ifstream io_file(std::format("/proc/{}/io", pid));
  if (!io_file.is_open()) return pio;

  std::string line;
  while (std::getline(io_file, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) continue;

    std::string_view key(line.data(), colon);
    std::string_view val_str(line.data() + colon + 1);

    // Trim leading whitespace from value
    auto start = val_str.find_first_not_of(" \t");
    if (start != std::string::npos) {
      val_str = val_str.substr(start);
    } else {
      continue;
    }

    unsigned long long val = 0;
    std::from_chars(val_str.data(), val_str.data() + val_str.size(), val);

    if (key == "rchar")                pio.rchar = val;
    else if (key == "wchar")           pio.wchar = val;
    else if (key == "read_bytes")      pio.read_bytes = val;
    else if (key == "write_bytes")     pio.write_bytes = val;
    else if (key == "cancelled_write_bytes") pio.cancelled_write_bytes = val;
  }

  return pio;
}

// ── Read /proc/[pid]/comm ──
std::string read_process_comm(int pid) {
  std::ifstream comm_file(std::format("/proc/{}/comm", pid));
  if (!comm_file.is_open()) return "?";

  std::string name;
  std::getline(comm_file, name);

  // Trim trailing newline
  while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
    name.pop_back();
  }
  // Truncate long names for display
  if (name.size() > 24) {
    name.resize(24);
  }
  return name;
}

// ── Color helper ──
struct Color {
  static constexpr const char* reset   = "\033[0m";
  static constexpr const char* bold    = "\033[1m";
  static constexpr const char* dim     = "\033[2m";
  static constexpr const char* cyan    = "\033[36m";
  static constexpr const char* green   = "\033[32m";
  static constexpr const char* yellow  = "\033[33m";
  static constexpr const char* red     = "\033[31m";
  static constexpr const char* blue    = "\033[34m";
  static constexpr const char* magenta = "\033[35m";
};

// ── Print formatted table ──
void print_table(const procio::IOSnapshot& snap, const procio::Options& opts) {
  bool c = opts.ansi_color;

  // ── Determine sort order ──
  auto& procs = const_cast<std::vector<procio::ProcessIO>&>(snap.processes);
  if (opts.sort_by == "rchar") {
    std::ranges::sort(procs, std::greater{}, &procio::ProcessIO::rchar);
  } else if (opts.sort_by == "wchar") {
    std::ranges::sort(procs, std::greater{}, &procio::ProcessIO::wchar);
  } else if (opts.sort_by == "read") {
    std::ranges::sort(procs, std::greater{}, &procio::ProcessIO::read_bytes);
  } else if (opts.sort_by == "write") {
    std::ranges::sort(procs, std::greater{}, &procio::ProcessIO::write_bytes);
  } else {
    // Default: sort by total storage I/O
    std::ranges::sort(procs, std::greater{}, [](const auto& p) { return p.total_storage(); });
  }

  int max_rows = std::min(opts.max_rows, static_cast<int>(procs.size()));
  int displayed = 0;

  // ── Header ──
  if (c) std::print(std::cout, "{}", Color::bold);
  std::println(std::cout, " {:<6} {:<24} {:>11} {:>11} {:>11} {:>11} {:>13}",
               "PID", "COMM", "RCHAR", "WCHAR", "READ", "WRITE", "TOTAL I/O");
  if (c) std::print(std::cout, "{}", Color::reset);

  // Separator
  if (c) std::print(std::cout, "{}", Color::dim);
  std::println(std::cout, " {:-<6} {:-<24} {:->11} {:->11} {:->11} {:->11} {:->13}",
               "", "", "", "", "", "", "");
  if (c) std::print(std::cout, "{}", Color::reset);

  // ── Rows ──
  std::string_view color_cycle[] = {Color::cyan, Color::green, Color::yellow,
                                    Color::magenta, Color::blue};
  int num_colors = 5;

  for (int i = 0; i < max_rows; ++i) {
    const auto& p = procs[i];
    auto sto_total = p.total_storage();

    std::string_view row_color = (c) ? color_cycle[i % num_colors] : std::string_view{};

    if (c) std::print(std::cout, "{}", row_color);
    std::println(std::cout, " {:<6} {:<24} {:>11} {:>11} {:>11} {:>11} {:>13}",
                 p.pid, p.comm,
                 fmt_num(p.rchar), fmt_num(p.wchar),
                 fmt_bytes(p.read_bytes), fmt_bytes(p.write_bytes),
                 fmt_bytes(sto_total));
    if (c) std::print(std::cout, "{}", Color::reset);

    displayed++;
  }

  // ── Summary ──
  if (c) std::print(std::cout, "{}", Color::dim);
  std::println(std::cout, " {:-<6} {:-<24} {:->11} {:->11} {:->11} {:->11} {:->13}",
               "", "", "", "", "", "", "");
  if (c) std::print(std::cout, "{}", Color::reset);
  if (c) std::print(std::cout, "{}", Color::bold);
  std::println(std::cout, " {:<6} {:<24} {:>11} {:>11} {:>11} {:>11} {:>13}",
               "TOTAL", std::to_string(procs.size()) + " procs",
               fmt_num(snap.total_rchar), fmt_num(snap.total_wchar),
               fmt_bytes(snap.total_read_bytes), fmt_bytes(snap.total_write_bytes),
               fmt_bytes(snap.total_read_bytes + snap.total_write_bytes));
  if (c) std::print(std::cout, "{}", Color::reset);

  std::println(std::cout, "");
  if (c) std::print(std::cout, "{}", Color::dim);
  std::println(std::cout, " PID filter: {}  |  Sort: {}  |  {} processes",
               opts.pid_filter > 0 ? std::to_string(opts.pid_filter) : "all",
               opts.sort_by, procs.size());
  if (opts.watch) {
    std::println(std::cout, " Watch mode: every {}s  (Ctrl+C to exit)", opts.interval);
  }
  if (c) std::print(std::cout, "{}", Color::reset);
}

// ── Print JSON output ──
void print_json(const procio::IOSnapshot& snap) {
  std::println(std::cout, "{{");
  std::println(std::cout, "  \"timestamp\": {},",
               std::chrono::system_clock::now().time_since_epoch().count());
  std::println(std::cout, "  \"process_count\": {},", snap.processes.size());
  std::println(std::cout, "  \"totals\": {{");
  std::println(std::cout, "    \"rchar\": {},", snap.total_rchar);
  std::println(std::cout, "    \"wchar\": {},", snap.total_wchar);
  std::println(std::cout, "    \"read_bytes\": {},", snap.total_read_bytes);
  std::println(std::cout, "    \"write_bytes\": {}", snap.total_write_bytes);
  std::println(std::cout, "  }},");
  std::println(std::cout, "  \"processes\": [");

  auto sorted = snap.processes;
  std::ranges::sort(sorted, std::greater{}, [](const auto& p) { return p.total_storage(); });
  int max_json = std::min(50, static_cast<int>(sorted.size()));

  for (int i = 0; i < max_json; ++i) {
    const auto& p = sorted[i];
    std::println(std::cout, "    {{");
    std::println(std::cout, "      \"pid\": {},", p.pid);
    std::println(std::cout, "      \"comm\": \"{}\",", p.comm);
    std::println(std::cout, "      \"rchar\": {},", p.rchar);
    std::println(std::cout, "      \"wchar\": {},", p.wchar);
    std::println(std::cout, "      \"read_bytes\": {},", p.read_bytes);
    std::println(std::cout, "      \"write_bytes\": {},", p.write_bytes);
    std::println(std::cout, "      \"cancelled_write_bytes\": {}", p.cancelled_write_bytes);
    bool last = (i == max_json - 1);
    if (last) {
      std::println(std::cout, "    }}");
    } else {
      std::println(std::cout, "    }},");
    }
  }

  std::println(std::cout, "  ]");
  std::println(std::cout, "}}");
}

// ── Format bytes with unit ──
std::string fmt_bytes(unsigned long long bytes) {
  if (bytes >= 1'000'000'000ULL) {
    return std::format("{:>6.1f}G", bytes / 1'000'000'000.0);
  } else if (bytes >= 1'000'000ULL) {
    return std::format("{:>6.1f}M", bytes / 1'000'000.0);
  } else if (bytes >= 1'000ULL) {
    return std::format("{:>6.1f}K", bytes / 1'000.0);
  } else {
    return std::format("{:>6}", bytes);
  }
}

// ── Format integer with thousand separators ──
std::string fmt_num(unsigned long long n) {
  std::string s = std::to_string(n);
  int insert_pos = static_cast<int>(s.size()) - 3;
  while (insert_pos > 0) {
    s.insert(static_cast<size_t>(insert_pos), ",");
    insert_pos -= 3;
  }
  return s;
}

// ── Clear terminal screen ──
void clear_screen() {
  // ANSI escape: clear screen + home cursor
  std::print(std::cout, "\033[2J\033[H");
}
