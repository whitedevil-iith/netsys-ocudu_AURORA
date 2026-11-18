// AURORA E3 interface tests: AURORA_KPM service model encoding and, when built with ZeroMQ, an end-to-end E3AP
// exchange (setup, subscription, indications, delete, release) against a running AURORA instance, with this test
// playing the role of the E3 Manager.

#include "lib/aurora/e3/aurora_e3_sm.h"
#include "ocudu/aurora/aurora.h"
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <unistd.h>
#ifdef AURORA_E3_ENABLED
#include <zmq.h>
#endif

using namespace ocudu;
using namespace aurora;
using json = nlohmann::json;

namespace {

std::unique_ptr<aurora_metrics> make_snapshot()
{
  auto m = std::make_unique<aurora_metrics>();
  std::memset(m.get(), 0, sizeof(aurora_metrics));
  m->ran_type               = RAN_DU;
  m->collection_interval_ms = 100;
  m->timestamp_us           = 1234567;
  m->active_ue_count        = 1;
  m->sinr_avg               = 17.5F;
  std::snprintf(m->ext.anomaly_label, sizeof(m->ext.anomaly_label), "normal");
  m->kpm_metric_count              = 1;
  m->kpm_metrics[0].measurement_id = 7;
  std::snprintf(m->kpm_metrics[0].name, sizeof(m->kpm_metrics[0].name), "DRB.UEThpDl");
  m->kpm_metrics[0].value_type   = 1;
  m->kpm_metrics[0].value_real   = 42.5;
  m->mac_ue_stats[3].rnti        = 0x4601;
  m->mac_ue_stats[3].wb_cqi      = 12;
  m->ext.csi[3].rnti             = 0x4601;
  m->ext.csi[3].nof_rs           = 2;
  m->rlc_rb_stats[3][4].rnti     = 0x4601;
  m->rlc_rb_stats[3][4].rbid     = 4;
  m->gtp_tunnel_stats[5][0].rnti = 6; // A UE slot with only a GTP-U tunnel (CU-UP style key).
  return m;
}

} // namespace

TEST(aurora_e3_sm, ran_function_definition_lists_every_stream)
{
  json rf = e3sm::ran_function_definition();
  EXPECT_EQ(rf["ranFunctionIdentifier"], e3sm::ran_function_id);
  ASSERT_EQ(rf["telemetryIdentifierList"].size(), e3sm::nof_telemetry_ids);
  ASSERT_EQ(rf["ranFunctionData"].size(), e3sm::nof_telemetry_ids);
  for (uint32_t i = 0; i != e3sm::nof_telemetry_ids; ++i) {
    EXPECT_EQ(rf["ranFunctionData"][i]["telemetryIdentifier"], i + 1);
    EXPECT_EQ(rf["ranFunctionData"][i]["delivery_method"]["transport_type"], "protocolData");
  }
}

TEST(aurora_e3_sm, only_subscribed_streams_are_encoded)
{
  auto                 m = make_snapshot();
  e3sm::telemetry_mask mask;
  e3sm::add(mask, e3sm::telemetry::timestamp);
  e3sm::add(mask, e3sm::telemetry::cell);
  json pd = e3sm::encode_protocol_data(*m, mask);
  EXPECT_EQ(pd["timestamp_us"], 1234567U);
  EXPECT_FLOAT_EQ(pd["cell"]["sinr_avg"].get<float>(), 17.5F);
  EXPECT_FALSE(pd.contains("ues"));
  EXPECT_FALSE(pd.contains("kpm"));
  EXPECT_FALSE(pd.contains("anomaly_label"));
}

TEST(aurora_e3_sm, all_streams_encode_root_and_per_ue_data)
{
  auto                 m = make_snapshot();
  e3sm::telemetry_mask mask;
  mask.set();
  json pd = e3sm::encode_protocol_data(*m, mask);
  EXPECT_EQ(pd["anomaly_label"], "normal");
  EXPECT_EQ(pd["node"]["ran_type"], "du");
  ASSERT_EQ(pd["kpm"].size(), 1U);
  EXPECT_EQ(pd["kpm"][0]["name"], "DRB.UEThpDl");
  EXPECT_DOUBLE_EQ(pd["kpm"][0]["value"].get<double>(), 42.5);
  ASSERT_EQ(pd["ues"].size(), 2U);
  const json& ue = pd["ues"][0];
  EXPECT_EQ(ue["ue_slot"], 3U);
  EXPECT_EQ(ue["rnti"], 0x4601U);
  EXPECT_EQ(ue["mac"]["wb_cqi"], 12U);
  EXPECT_EQ(ue["csi"]["cri"].size(), 2U);
  ASSERT_EQ(ue["rlc"].size(), 1U);
  EXPECT_EQ(ue["rlc"][0]["rbid"], 4U);
  EXPECT_FALSE(ue.contains("gtp"));
  EXPECT_EQ(pd["ues"][1]["ue_slot"], 5U);
  EXPECT_EQ(pd["ues"][1]["rnti"], 6U);
  EXPECT_EQ(pd["ues"][1]["gtp"].size(), 1U);
  EXPECT_TRUE(pd["threads"].is_array());
  EXPECT_TRUE(pd["handover"]["du"].is_array());
  // Serialisable (no invalid UTF-8 from the fixed-size char arrays).
  EXPECT_FALSE(pd.dump().empty());
}

#ifdef AURORA_E3_ENABLED

namespace {

struct zmq_peer {
  void* ctx = zmq_ctx_new();
  void* req = zmq_socket(ctx, ZMQ_REQ);
  void* sub = zmq_socket(ctx, ZMQ_SUB);
  void* pub = zmq_socket(ctx, ZMQ_PUB);

  zmq_peer(uint16_t rep_port, uint16_t pub_port, uint16_t sub_port)
  {
    int timeout_ms = 3000;
    int linger     = 0;
    for (void* s : {req, sub, pub}) {
      zmq_setsockopt(s, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
      zmq_setsockopt(s, ZMQ_LINGER, &linger, sizeof(linger));
    }
    zmq_setsockopt(sub, ZMQ_SUBSCRIBE, "", 0);
    zmq_connect(req, ("tcp://127.0.0.1:" + std::to_string(rep_port)).c_str());
    zmq_connect(sub, ("tcp://127.0.0.1:" + std::to_string(pub_port)).c_str());
    zmq_connect(pub, ("tcp://127.0.0.1:" + std::to_string(sub_port)).c_str());
  }
  ~zmq_peer()
  {
    zmq_close(req);
    zmq_close(sub);
    zmq_close(pub);
    zmq_ctx_term(ctx);
  }

  static void send(void* s, const json& j)
  {
    std::string m = j.dump();
    zmq_send(s, m.data(), m.size(), 0);
  }

  static json recv(void* s)
  {
    char buf[1 << 20];
    int  n = zmq_recv(s, buf, sizeof(buf), 0);
    if (n < 0) {
      return json();
    }
    return json::parse(std::string(buf, std::min<size_t>(size_t(n), sizeof(buf))));
  }

  /// Next message of the given type on the SUB socket (indications may arrive before a response).
  json recv_type(const std::string& type)
  {
    for (int i = 0; i != 50; ++i) {
      json j = recv(sub);
      if (j.is_null()) {
        return j;
      }
      if (j.value("type", "") == type) {
        return j;
      }
    }
    return json();
  }
};

} // namespace

TEST(aurora_e3_agent, setup_subscribe_indicate_delete_release)
{
  const uint16_t base = uint16_t(20000 + (::getpid() % 20000));

  aurora::config cfg;
  cfg.enabled                = true;
  cfg.type                   = aurora::ran_type::du;
  cfg.collection_interval    = std::chrono::milliseconds{50};
  cfg.shm_key                = 0x7c000000U | (static_cast<uint32_t>(::getpid()) & 0xffffffU);
  cfg.thread_metrics_enabled = false;
  cfg.anomaly_enabled        = false;
  cfg.e3_enabled             = true;
  cfg.e3_rep_port            = base;
  cfg.e3_pub_port            = base + 1;
  cfg.e3_sub_port            = base + 2;
  ASSERT_EQ(aurora::start(cfg), 0);

  {
    zmq_peer mgr(cfg.e3_rep_port, cfg.e3_pub_port, cfg.e3_sub_port);

    // Wrong protocol version is rejected.
    zmq_peer::send(mgr.req, {{"type", "setupRequest"}, {"id", 1}, {"e3apProtocolVersion", "0.9.0"}});
    json bad = zmq_peer::recv(mgr.req);
    EXPECT_EQ(bad["responseCode"], "negative");

    zmq_peer::send(mgr.req,
                   {{"type", "setupRequest"},
                    {"id", 2},
                    {"e3apProtocolVersion", "1.0.0"},
                    {"dAppName", "aurora_e3_test"},
                    {"dAppVersion", "1.0.0"},
                    {"vendor", "test"}});
    json setup = zmq_peer::recv(mgr.req);
    ASSERT_EQ(setup["responseCode"], "positive");
    EXPECT_EQ(setup["ranIdentifier"], "OCUDU_DU");
    ASSERT_EQ(setup["ranFunctionList"].size(), 1U);
    EXPECT_EQ(setup["ranFunctionList"][0]["ranFunctionIdentifier"], e3sm::ran_function_id);
    const uint32_t dapp_id = setup["dAppIdentifier"];

    // PUB/SUB slow joiner: give both connections time to complete before publishing.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    // Unknown RAN function (NVIDIA KPM) is rejected.
    zmq_peer::send(mgr.pub,
                   {{"type", "subscriptionRequest"},
                    {"id", 3},
                    {"dAppIdentifier", dapp_id},
                    {"ranFunctionIdentifier", 2},
                    {"telemetryIdentifierList", {1}}});
    json rej = mgr.recv_type("subscriptionResponse");
    EXPECT_EQ(rej["responseCode"], "negative");

    aurora::record_flow(aurora::flow::f1u_rlc, 1000);
    zmq_peer::send(mgr.pub,
                   {{"type", "subscriptionRequest"},
                    {"id", 4},
                    {"dAppIdentifier", dapp_id},
                    {"ranFunctionIdentifier", e3sm::ran_function_id},
                    {"telemetryIdentifierList", {1, 3, 4, 8}},
                    {"periodicity", 50000}});
    json sub = mgr.recv_type("subscriptionResponse");
    ASSERT_EQ(sub["responseCode"], "positive");
    EXPECT_EQ(sub["requestId"], 4);
    const uint32_t sub_id = sub["subscriptionId"];

    json ind = mgr.recv_type("indicationMessage");
    ASSERT_FALSE(ind.is_null());
    EXPECT_EQ(ind["dAppIdentifier"], dapp_id);
    EXPECT_EQ(ind["subscriptionId"], sub_id);
    EXPECT_EQ(ind["ranFunctionIdentifier"], e3sm::ran_function_id);
    const json& pd = ind["protocolData"];
    EXPECT_TRUE(pd.contains("timestamp_us"));
    EXPECT_EQ(pd["anomaly_label"], "normal");
    EXPECT_TRUE(pd.contains("cell"));
    EXPECT_TRUE(pd.contains("flows"));
    EXPECT_FALSE(pd.contains("ues"));

    // Indications keep coming at the subscribed periodicity, with increasing timestamps.
    json ind2 = mgr.recv_type("indicationMessage");
    ASSERT_FALSE(ind2.is_null());
    EXPECT_GT(ind2["protocolData"]["timestamp_us"].get<uint64_t>(), pd["timestamp_us"].get<uint64_t>());

    zmq_peer::send(
        mgr.pub, {{"type", "subscriptionDelete"}, {"id", 5}, {"dAppIdentifier", dapp_id}, {"subscriptionId", sub_id}});
    json del = mgr.recv_type("subscriptionResponse");
    EXPECT_EQ(del["responseCode"], "positive");

    zmq_peer::send(mgr.pub, {{"type", "releaseMessage"}, {"id", 6}, {"dAppIdentifier", dapp_id}});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    // Released: a new subscription for the old dApp id is ignored (no response at all).
    zmq_peer::send(mgr.pub,
                   {{"type", "subscriptionRequest"},
                    {"id", 7},
                    {"dAppIdentifier", dapp_id},
                    {"ranFunctionIdentifier", e3sm::ran_function_id},
                    {"telemetryIdentifierList", {1}}});
    EXPECT_TRUE(mgr.recv_type("subscriptionResponse").is_null());
  }
  aurora::stop();
}

#endif // AURORA_E3_ENABLED
