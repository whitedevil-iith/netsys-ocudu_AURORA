// AURORA E3 agent. See aurora_e3_agent.h.

#include "aurora_e3_agent.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <zmq.h>

using namespace ocudu;
using namespace aurora;
using namespace aurora::e3;
using json = nlohmann::json;

namespace {

constexpr std::chrono::milliseconds poll_period{100};
constexpr std::chrono::seconds      reap_period{1};
constexpr int                       pub_linger_ms = 200;

bool recv_string(void* sock, std::string& out)
{
  zmq_msg_t msg;
  zmq_msg_init(&msg);
  int n = zmq_msg_recv(&msg, sock, ZMQ_DONTWAIT);
  if (n < 0) {
    zmq_msg_close(&msg);
    return false;
  }
  out.assign(static_cast<const char*>(zmq_msg_data(&msg)), zmq_msg_size(&msg));
  zmq_msg_close(&msg);
  return true;
}

void* bind_socket(void* ctx, int type, const std::string& address, uint16_t port)
{
  void* s = zmq_socket(ctx, type);
  if (s == nullptr) {
    return nullptr;
  }
  std::string endpoint = "tcp://" + address + ":" + std::to_string(port);
  if (zmq_bind(s, endpoint.c_str()) != 0) {
    std::fprintf(stderr, "AURORA E3: cannot bind %s: %s\n", endpoint.c_str(), zmq_strerror(zmq_errno()));
    zmq_close(s);
    return nullptr;
  }
  return s;
}

void set_int(void* s, int opt, int value)
{
  zmq_setsockopt(s, opt, &value, sizeof(value));
}

json negative(const char* type, uint32_t msg_id, uint32_t request_id, uint32_t dapp_id, const char* reason)
{
  json r;
  r["type"]           = type;
  r["id"]             = msg_id;
  r["requestId"]      = request_id;
  r["dAppIdentifier"] = dapp_id;
  r["responseCode"]   = "negative";
  r["message"]        = reason;
  return r;
}

} // namespace

agent::~agent()
{
  stop();
}

bool agent::start(const agent_config& config)
{
  if (running.load()) {
    return true;
  }
  cfg = config;
  ctx = zmq_ctx_new();
  if (ctx == nullptr) {
    std::fprintf(stderr, "AURORA E3: zmq_ctx_new failed\n");
    return false;
  }
  rep_sock = bind_socket(ctx, ZMQ_REP, cfg.bind_address, cfg.rep_port);
  pub_sock = bind_socket(ctx, ZMQ_PUB, cfg.bind_address, cfg.pub_port);
  sub_sock = bind_socket(ctx, ZMQ_SUB, cfg.bind_address, cfg.sub_port);
  if (rep_sock == nullptr || pub_sock == nullptr || sub_sock == nullptr) {
    for (void** s : {&rep_sock, &pub_sock, &sub_sock}) {
      if (*s != nullptr) {
        zmq_close(*s);
        *s = nullptr;
      }
    }
    zmq_ctx_term(ctx);
    ctx = nullptr;
    return false;
  }
  zmq_setsockopt(sub_sock, ZMQ_SUBSCRIBE, "", 0);
  set_int(pub_sock, ZMQ_LINGER, pub_linger_ms);
  set_int(rep_sock, ZMQ_LINGER, 0);
  set_int(sub_sock, ZMQ_LINGER, 0);
  // Same keepalive settings as the cuBB agent: detect a vanished E3 Manager.
  set_int(rep_sock, ZMQ_TCP_KEEPALIVE, 1);
  set_int(rep_sock, ZMQ_TCP_KEEPALIVE_IDLE, 5);
  set_int(rep_sock, ZMQ_TCP_KEEPALIVE_INTVL, 2);
  set_int(rep_sock, ZMQ_TCP_KEEPALIVE_CNT, 3);

  running.store(true);
  worker = std::thread(&agent::run, this);
  std::fprintf(stdout,
               "AURORA E3: agent '%s' listening on %s (REP %u, PUB %u, SUB %u), RAN function %u (%s)\n",
               cfg.ran_identifier.c_str(),
               cfg.bind_address.c_str(),
               unsigned(cfg.rep_port),
               unsigned(cfg.pub_port),
               unsigned(cfg.sub_port),
               e3sm::ran_function_id,
               std::string(e3sm::ran_function_name).c_str());
  return true;
}

void agent::stop()
{
  if (!running.exchange(false)) {
    return;
  }
  if (worker.joinable()) {
    worker.join();
  }
  // Tell the E3 Manager that its dApps are gone, so that it can set up again once the RAN process is back.
  std::vector<uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (const auto& d : dapps) {
      ids.push_back(d.first);
    }
  }
  for (uint32_t id : ids) {
    send_release(id);
  }
  {
    std::lock_guard<std::mutex> lock(pub_mutex);
    for (void** s : {&rep_sock, &pub_sock, &sub_sock}) {
      if (*s != nullptr) {
        zmq_close(*s);
        *s = nullptr;
      }
    }
  }
  zmq_ctx_term(ctx);
  ctx = nullptr;
}

size_t agent::nof_subscriptions() const
{
  std::lock_guard<std::mutex> lock(state_mutex);
  return subscriptions.size();
}

void agent::run()
{
  ::pthread_setname_np(::pthread_self(), "aurora_e3");
  if (!cfg.cpus.empty()) {
    ::cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu : cfg.cpus) {
      if (cpu < CPU_SETSIZE) {
        CPU_SET(cpu, &set);
      }
    }
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
  }

  auto next_reap = std::chrono::steady_clock::now() + reap_period;
  while (running.load(std::memory_order_relaxed)) {
    zmq_pollitem_t items[] = {{rep_sock, 0, ZMQ_POLLIN, 0}, {sub_sock, 0, ZMQ_POLLIN, 0}};
    int            rc      = zmq_poll(items, 2, long(poll_period.count()));
    if (rc < 0 && zmq_errno() != EINTR) {
      std::fprintf(stderr, "AURORA E3: zmq_poll failed: %s\n", zmq_strerror(zmq_errno()));
      break;
    }
    std::string msg;
    if ((items[0].revents & ZMQ_POLLIN) != 0 && recv_string(rep_sock, msg)) {
      std::string reply = handle_request(msg);
      zmq_send(rep_sock, reply.data(), reply.size(), 0);
    }
    // Drain the SUB socket at arrival rate.
    while ((items[1].revents & ZMQ_POLLIN) != 0 && recv_string(sub_sock, msg)) {
      handle_manager_message(msg);
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= next_reap) {
      reap();
      next_reap = now + reap_period;
    }
  }
}

void agent::publish(const std::string& msg)
{
  std::lock_guard<std::mutex> lock(pub_mutex);
  if (pub_sock != nullptr) {
    // Fire and forget, like the cuBB agent: never block the publisher on a slow or absent E3 Manager.
    zmq_send(pub_sock, msg.data(), msg.size(), ZMQ_DONTWAIT);
  }
}

std::string agent::handle_request(const std::string& msg)
{
  json req;
  try {
    req = json::parse(msg);
  } catch (const json::exception&) {
    json r;
    r["responseCode"] = "negative";
    r["message"]      = "invalid JSON format";
    return r.dump();
  }
  const std::string type = req.value("type", "");
  if (type == "setupRequest") {
    return handle_setup_request(req);
  }
  return negative(type.c_str(), next_msg_id++, req.value("id", 0u), 0, "unknown request type").dump();
}

void agent::handle_manager_message(const std::string& msg)
{
  json m;
  try {
    m = json::parse(msg);
  } catch (const json::exception&) {
    std::fprintf(stderr, "AURORA E3: dropping a malformed E3 Manager message\n");
    return;
  }
  const std::string type    = m.value("type", "");
  const uint32_t    dapp_id = m.value("dAppIdentifier", 0u);

  if (type == "releaseMessage") {
    release(dapp_id);
    return;
  }
  // The E3 Manager publishes to every agent it is connected to: ignore dApps set up through another agent.
  if (!is_connected(dapp_id)) {
    return;
  }
  if (type == "subscriptionRequest") {
    publish(handle_subscription_request(m));
  } else if (type == "subscriptionDelete") {
    publish(handle_subscription_delete(m));
  } else if (type == "dAppControlAction") {
    publish(handle_control_action(m));
  }
}

std::string agent::handle_setup_request(const json& req)
{
  const uint32_t    request_id = req.value("id", 0u);
  const std::string version    = req.value("e3apProtocolVersion", "");
  if (version != e3sm::e3ap_protocol_version) {
    json r                   = negative("setupResponse", next_msg_id++, request_id, 0, "protocol version mismatch");
    r["e3apProtocolVersion"] = e3sm::e3ap_protocol_version;
    return r.dump();
  }

  const uint32_t dapp_id = next_dapp_id++;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    dapps[dapp_id] = std::chrono::steady_clock::now();
  }
  std::fprintf(stdout,
               "AURORA E3: setup of dApp '%s' v%s (%s), assigned dApp id %u\n",
               req.value("dAppName", "unknown").c_str(),
               req.value("dAppVersion", "unknown").c_str(),
               req.value("vendor", "unknown").c_str(),
               dapp_id);

  json r;
  r["type"]                = "setupResponse";
  r["id"]                  = next_msg_id++;
  r["requestId"]           = request_id;
  r["responseCode"]        = "positive";
  r["e3apProtocolVersion"] = e3sm::e3ap_protocol_version;
  r["dAppIdentifier"]      = dapp_id;
  r["ranIdentifier"]       = cfg.ran_identifier;
  r["ranFunctionList"]     = json::array({e3sm::ran_function_definition()});
  return r.dump();
}

std::string agent::handle_subscription_request(const json& req)
{
  const uint32_t request_id = req.value("id", 0u);
  const uint32_t dapp_id    = req.value("dAppIdentifier", 0u);
  const char*    type       = "subscriptionResponse";

  if (!touch(dapp_id)) {
    return negative(type, next_msg_id++, request_id, dapp_id, "dApp not connected or timed out").dump();
  }
  std::vector<uint32_t> ids;
  uint32_t              ran_function   = 0;
  uint32_t              periodicity_us = uint32_t(std::chrono::microseconds(cfg.collection_interval).count());
  uint32_t              duration_s     = 0;
  try {
    ran_function   = req.at("ranFunctionIdentifier").get<uint32_t>();
    ids            = req.value("telemetryIdentifierList", std::vector<uint32_t>{});
    periodicity_us = req.value("periodicity", periodicity_us);
    duration_s     = req.value("subscriptionTime", 0u);
  } catch (const json::exception&) {
    return negative(type, next_msg_id++, request_id, dapp_id, "missing or invalid parameters in subscription request")
        .dump();
  }
  if (ran_function != e3sm::ran_function_id) {
    return negative(type, next_msg_id++, request_id, dapp_id, "unsupported ranFunctionIdentifier").dump();
  }
  // Telemetry only: the AURORA service model defines no control actions.
  if (ids.empty()) {
    return negative(type, next_msg_id++, request_id, dapp_id, "telemetryIdentifierList must not be empty").dump();
  }
  e3sm::telemetry_mask mask;
  for (uint32_t id : ids) {
    if (!e3sm::is_valid_telemetry_id(id)) {
      return negative(type, next_msg_id++, request_id, dapp_id, "invalid telemetry identifier").dump();
    }
    mask.set(id - 1);
  }

  const auto   now = std::chrono::steady_clock::now();
  subscription sub;
  sub.id            = next_sub_id++;
  sub.dapp_id       = dapp_id;
  sub.telemetry_ids = ids;
  sub.mask          = mask;
  sub.periodicity   = std::chrono::microseconds(periodicity_us);
  // First indication on the next interval.
  sub.last_sent = now - sub.periodicity;
  sub.expiry = duration_s > 0 ? now + std::chrono::seconds(duration_s) : std::chrono::steady_clock::time_point::max();
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    subscriptions[sub.id] = sub;
  }
  std::fprintf(stdout,
               "AURORA E3: subscription %u for dApp %u (%zu streams, periodicity %u us)\n",
               sub.id,
               dapp_id,
               ids.size(),
               periodicity_us);

  json r;
  r["type"]                  = type;
  r["id"]                    = next_msg_id++;
  r["requestId"]             = request_id;
  r["dAppIdentifier"]        = dapp_id;
  r["responseCode"]          = "positive";
  r["subscriptionId"]        = sub.id;
  r["ranFunctionIdentifier"] = e3sm::ran_function_id;
  r["telemetryGrantedList"]  = ids;
  r["controlGrantedList"]    = json::array();
  r["periodicity"]           = periodicity_us;
  return r.dump();
}

std::string agent::handle_subscription_delete(const json& req)
{
  const uint32_t request_id = req.value("id", 0u);
  const uint32_t dapp_id    = req.value("dAppIdentifier", 0u);
  const uint32_t sub_id     = req.value("subscriptionId", 0u);
  bool           found      = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto                        it = subscriptions.find(sub_id);
    if (it != subscriptions.end() && it->second.dapp_id == dapp_id) {
      subscriptions.erase(it);
      found = true;
    }
  }
  touch(dapp_id);

  json r              = found
                            ? json::object()
                            : negative("subscriptionResponse", 0, request_id, dapp_id, "subscription not found or dApp ID mismatch");
  r["type"]           = "subscriptionResponse";
  r["id"]             = next_msg_id++;
  r["requestId"]      = request_id;
  r["dAppIdentifier"] = dapp_id;
  r["subscriptionId"] = sub_id;
  if (found) {
    r["responseCode"] = "positive";
  }
  return r.dump();
}

std::string agent::handle_control_action(const json& req)
{
  const uint32_t dapp_id = req.value("dAppIdentifier", 0u);
  touch(dapp_id);
  json r = negative("messageAck", next_msg_id++, req.value("id", 0u), dapp_id, "control actions not implemented");
  return r.dump();
}

bool agent::is_connected(uint32_t dapp_id)
{
  std::lock_guard<std::mutex> lock(state_mutex);
  return dapps.count(dapp_id) != 0;
}

bool agent::touch(uint32_t dapp_id)
{
  std::lock_guard<std::mutex> lock(state_mutex);
  auto                        it = dapps.find(dapp_id);
  if (it == dapps.end()) {
    return false;
  }
  it->second = std::chrono::steady_clock::now();
  return true;
}

void agent::release(uint32_t dapp_id)
{
  std::lock_guard<std::mutex> lock(state_mutex);
  if (dapps.erase(dapp_id) == 0) {
    return;
  }
  for (auto it = subscriptions.begin(); it != subscriptions.end();) {
    it = it->second.dapp_id == dapp_id ? subscriptions.erase(it) : std::next(it);
  }
  std::fprintf(stdout, "AURORA E3: dApp %u released\n", dapp_id);
}

void agent::send_release(uint32_t dapp_id)
{
  json r;
  r["type"]           = "releaseMessage";
  r["id"]             = next_msg_id++;
  r["dAppIdentifier"] = dapp_id;
  publish(r.dump());
  release(dapp_id);
}

void agent::reap()
{
  const auto            now = std::chrono::steady_clock::now();
  std::vector<uint32_t> idle;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (auto it = subscriptions.begin(); it != subscriptions.end();) {
      if (now >= it->second.expiry) {
        std::fprintf(stdout, "AURORA E3: subscription %u of dApp %u expired\n", it->first, it->second.dapp_id);
        it = subscriptions.erase(it);
      } else {
        ++it;
      }
    }
    // Indications are fire and forget, so a dApp with subscriptions is kept even when silent (as in the cuBB agent).
    for (const auto& d : dapps) {
      bool has_sub = false;
      for (const auto& s : subscriptions) {
        has_sub = has_sub || s.second.dapp_id == d.first;
      }
      if (!has_sub && now - d.second > cfg.inactivity_timeout) {
        idle.push_back(d.first);
      }
    }
  }
  for (uint32_t id : idle) {
    send_release(id);
  }
}

void agent::on_interval(const aurora_metrics& m)
{
  if (!running.load(std::memory_order_relaxed)) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // Indications only go out on interval boundaries: accept half an interval of jitter so that a periodicity equal
  // to (or a multiple of) the collection interval does not skip every other interval.
  const auto tolerance = std::chrono::duration_cast<std::chrono::microseconds>(cfg.collection_interval) / 2;

  struct pending {
    uint32_t sub_id;
    uint32_t dapp_id;
    size_t   payload;
  };
  std::vector<pending>                                    due;
  std::vector<std::pair<unsigned long long, std::string>> payloads; // Encoded once per distinct stream set.
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    for (auto& entry : subscriptions) {
      subscription& s = entry.second;
      if (now - s.last_sent + tolerance < s.periodicity) {
        continue;
      }
      s.last_sent            = now;
      unsigned long long key = s.mask.to_ullong();
      size_t             idx = payloads.size();
      for (size_t i = 0; i != payloads.size(); ++i) {
        if (payloads[i].first == key) {
          idx = i;
          break;
        }
      }
      if (idx == payloads.size()) {
        payloads.emplace_back(key, std::string());
      }
      due.push_back({s.id, s.dapp_id, idx});
    }
  }
  if (due.empty()) {
    return;
  }
  for (auto& p : payloads) {
    p.second = e3sm::encode_protocol_data(m, e3sm::telemetry_mask(p.first)).dump();
  }
  for (const pending& d : due) {
    // Build the envelope by hand: protocolData is already serialised and can be large.
    std::string msg;
    msg.reserve(payloads[d.payload].second.size() + 160);
    msg += "{\"type\":\"indicationMessage\",\"id\":";
    msg += std::to_string(next_msg_id++);
    msg += ",\"dAppIdentifier\":";
    msg += std::to_string(d.dapp_id);
    msg += ",\"ranFunctionIdentifier\":";
    msg += std::to_string(e3sm::ran_function_id);
    msg += ",\"subscriptionId\":";
    msg += std::to_string(d.sub_id);
    msg += ",\"protocolData\":";
    msg += payloads[d.payload].second;
    msg += '}';
    publish(msg);
  }
}
