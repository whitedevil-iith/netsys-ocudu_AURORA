#pragma once

#include "apps/services/worker_manager/os_sched_affinity_manager.h"
#include <chrono>
#include <cstdint>
#include <string>

namespace ocudu {
namespace app_services {

/// AURORA metrics framework application configuration (YAML section "aurora").
struct aurora_appconfig {
  /// Enables the AURORA shared-memory metrics framework.
  bool enabled = false;
  /// Interval at which the shared memory snapshot is published.
  std::chrono::milliseconds collection_interval{100};
  /// SysV shared memory key, hexadecimal string (e.g. "0xDEADBEEF"). Must be unique per process on a host.
  std::string shm_key = "0xDEADBEEF";
  /// RAN node type reported to readers: auto (derived from the application), gnb, du, cu, cu_cp or cu_up.
  std::string ran_type = "auto";
  /// Per-thread runtime distribution metrics.
  bool thread_metrics_enabled = true;
  /// Thread metrics backend: auto (eBPF, falling back to procfs), ebpf or procfs.
  std::string thread_metrics_backend = "auto";
  /// Comma-separated thread names to monitor ('*' suffix = prefix match). Empty selects the OCUDU default list.
  std::string thread_metrics_whitelist;
  /// Anomaly-injection TCP control server on 127.0.0.1:(47010 + ran_type).
  bool anomaly_enabled = true;
  /// CPUs of the AURORA publisher thread ("aurora_reset"). Empty uses the main thread pool CPUs
  /// (expert_execution.affinities.main_pool_cpus), so the publisher stays off the dedicated cores.
  os_sched_affinity_bitmask cpus;
  /// E3 agent serving the AURORA_KPM service model to dApps (E3 Manager of NVIDIA aerial-sample-apps).
  bool e3_enabled = false;
  /// Bind address of the E3 sockets ("*" = all interfaces).
  std::string e3_bind_address = "127.0.0.1";
  /// E3 Manager setup requests (REQ/REP).
  uint16_t e3_rep_port = 9990;
  /// Indications and responses to the E3 Manager (PUB).
  uint16_t e3_pub_port = 9991;
  /// Subscription, control and release messages from the E3 Manager (SUB).
  uint16_t e3_sub_port = 9999;
};

} // namespace app_services
} // namespace ocudu
