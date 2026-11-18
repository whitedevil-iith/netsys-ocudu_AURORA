// AURORA E3 agent: serves the AURORA service model (aurora_e3_sm.h) to dApps over the E3 interface.
//
// Wire protocol and socket roles are those of the cuBB E3 agent (cuPHY-CP/data_lake/e3_agent.*) and of the
// aerial-sample-apps E3 Manager, so an E3 Manager connects to AURORA as one more entry of its "e3_agents" list:
//
//   REP  (rep_port) : E3 Manager REQ -> agent. Setup Request / Setup Response.
//   PUB  (pub_port) : agent -> E3 Manager SUB. Subscription Responses, Indication Messages, Release, Message Ack.
//   SUB  (sub_port) : E3 Manager PUB -> agent. Subscription Request/Delete, dApp Control Action, Release.
//
// All messages are JSON (docs/e3_message_schemas.json of aerial-sample-apps). Indications are sent from the AURORA
// publisher thread right after each collection interval has been written to shared memory, so they carry exactly
// the snapshot that SysV-SHM readers see. One internal thread ("aurora_e3") serves the REP and SUB sockets and
// expires subscriptions; nothing runs on the RAN threads.

#pragma once

#include "aurora_e3_sm.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ocudu {
namespace aurora {
namespace e3 {

struct agent_config {
  /// Address the three sockets bind to ("*" for all interfaces).
  std::string bind_address = "127.0.0.1";
  uint16_t    rep_port     = 9990;
  uint16_t    pub_port     = 9991;
  uint16_t    sub_port     = 9999;
  /// ranIdentifier of the E3 Setup Response (e.g. "OCUDU_DU").
  std::string ran_identifier = "OCUDU";
  /// AURORA collection interval; indications cannot be produced faster than this.
  std::chrono::milliseconds collection_interval{100};
  /// A dApp without subscriptions and without any message for this long is released.
  std::chrono::seconds inactivity_timeout{1800};
  /// CPUs of the agent thread. Empty leaves it unpinned.
  std::vector<unsigned> cpus;
};

class agent
{
public:
  agent() = default;
  ~agent();
  agent(const agent&)            = delete;
  agent& operator=(const agent&) = delete;

  /// Binds the sockets and starts the agent thread. Returns false (and leaves nothing running) on failure.
  bool start(const agent_config& cfg);
  /// Releases every dApp, stops the agent thread and closes the sockets.
  void stop();

  /// Sends an Indication Message to every subscription whose periodicity has elapsed. Called by the AURORA
  /// publisher thread with the snapshot it has just published.
  void on_interval(const aurora_metrics& m);

  /// Number of active subscriptions (for tests and logs).
  size_t nof_subscriptions() const;

private:
  struct subscription {
    uint32_t                              id;
    uint32_t                              dapp_id;
    std::vector<uint32_t>                 telemetry_ids;
    e3sm::telemetry_mask                  mask;
    std::chrono::microseconds             periodicity;
    std::chrono::steady_clock::time_point last_sent;
    std::chrono::steady_clock::time_point expiry;
  };

  void        run();
  std::string handle_request(const std::string& msg);
  void        handle_manager_message(const std::string& msg);
  std::string handle_setup_request(const nlohmann::json& req);
  std::string handle_subscription_request(const nlohmann::json& req);
  std::string handle_subscription_delete(const nlohmann::json& req);
  std::string handle_control_action(const nlohmann::json& req);
  bool        is_connected(uint32_t dapp_id);
  bool        touch(uint32_t dapp_id);
  void        release(uint32_t dapp_id);
  void        send_release(uint32_t dapp_id);
  void        reap();
  void        publish(const std::string& msg);

  agent_config cfg;

  void* ctx      = nullptr;
  void* rep_sock = nullptr;
  void* pub_sock = nullptr;
  void* sub_sock = nullptr;

  std::thread       worker;
  std::atomic<bool> running{false};

  std::mutex pub_mutex;

  mutable std::mutex                                        state_mutex;
  std::map<uint32_t, std::chrono::steady_clock::time_point> dapps; ///< dApp id -> last activity.
  std::map<uint32_t, subscription>                          subscriptions;

  std::atomic<uint32_t> next_msg_id{1};
  std::atomic<uint32_t> next_dapp_id{1};
  std::atomic<uint32_t> next_sub_id{1};
};

} // namespace e3
} // namespace aurora
} // namespace ocudu
