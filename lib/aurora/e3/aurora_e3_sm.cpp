// AURORA E3 service model ("AURORA_KPM"): stream descriptors and snapshot encoder. See aurora_e3_sm.h.

#include "aurora_e3_sm.h"
#include <cstring>
#include <string>

using namespace ocudu;
using namespace aurora;
using json = nlohmann::json;

#define AURORA_E3_FIELD(obj, src, name) (obj)[#name] = (src).name

namespace {

struct stream_desc {
  e3sm::telemetry  id;
  std::string_view stream_id;
  std::string_view data_type;
  std::string_view description;
};

// Keep in telemetry identifier order.
constexpr stream_desc streams[] = {
    {e3sm::telemetry::timestamp, "timestamp_us", "uint64", "Snapshot wall clock, microseconds since epoch"},
    {e3sm::telemetry::node, "node", "object", "RAN node type, collection interval (ms) and snapshot sequence"},
    {e3sm::telemetry::anomaly_label, "anomaly_label", "string", "Label set by the anomaly-injector sidecar"},
    {e3sm::telemetry::cell,
     "cell",
     "object",
     "Cell-level KPIs: HARQ/CRC loss, SINR/CQI/RI/TA/BSR min-max-avg, PRB utilisation, scheduler loop, UE churn"},
    {e3sm::telemetry::kpm, "kpm", "array(object)", "O-RAN KPM measurements {measurement_id, name, value}"},
    {e3sm::telemetry::rc_cells,
     "rc_cells",
     "array(object)",
     "Radio-control cell stats: RSRP, SINR, load, interference level, handover counters"},
    {e3sm::telemetry::fapi, "fapi", "object", "FAPI PDSCH/PUSCH counts and MCS/PRB/TBS min-max-avg"},
    {e3sm::telemetry::flows,
     "flows",
     "object",
     "Interval byte counters of the BH/MH/F1-U/RLC/MAC data flows and MAC DL buffer occupancy"},
    {e3sm::telemetry::histograms, "hist", "object", "Packet-size and FAPI MCS/PRB histograms (16 bins)"},
    {e3sm::telemetry::ue_mac, "ue_mac", "object", "MAC scheduler stats [per-UE, ues[].mac]"},
    {e3sm::telemetry::ue_fapi, "ue_fapi", "object", "Last DL/UL FAPI PDU MCS/PRB/TBS [per-UE, ues[].fapi]"},
    {e3sm::telemetry::ue_csi, "ue_csi", "object", "Content of the last CSI report [per-UE, ues[].csi]"},
    {e3sm::telemetry::ue_rlc, "ue_rlc", "array(object)", "RLC stats per radio bearer [per-UE, ues[].rlc]"},
    {e3sm::telemetry::ue_pdcp, "ue_pdcp", "array(object)", "PDCP stats per radio bearer [per-UE, ues[].pdcp]"},
    {e3sm::telemetry::ue_gtp, "ue_gtp", "array(object)", "GTP-U stats per tunnel [per-UE, ues[].gtp]"},
    {e3sm::telemetry::f1c, "f1c", "object", "F1AP message counters and F1 setup / UE context procedure stats"},
    {e3sm::telemetry::f1u, "f1u", "object", "NR-U PDU, drop and delivery-status counters"},
    {e3sm::telemetry::handover, "handover", "object", "Handover executions per source DU and preparations"},
    {e3sm::telemetry::process, "process", "object", "getrusage() of the RAN process"},
    {e3sm::telemetry::threads, "threads", "array(object)", "Per-thread runtime distribution metrics"},
};
static_assert(sizeof(streams) / sizeof(streams[0]) == e3sm::nof_telemetry_ids, "one descriptor per telemetry id");

std::string c_string(const char* s, size_t max_len)
{
  return std::string(s, ::strnlen(s, max_len));
}

const char* ran_type_name(ran_type_t t)
{
  switch (t) {
    case RAN_GNB:
      return "gnb";
    case RAN_DU:
      return "du";
    case RAN_CU:
      return "cu";
    case RAN_CU_CP:
      return "cu_cp";
    case RAN_CU_UP:
      return "cu_up";
  }
  return "unknown";
}

json encode_hist(const aurora_hist_t& h)
{
  json o;
  o["bins"] = h.bins;
  AURORA_E3_FIELD(o, h, count);
  AURORA_E3_FIELD(o, h, min_val);
  AURORA_E3_FIELD(o, h, max_val);
  AURORA_E3_FIELD(o, h, sum);
  return o;
}

json encode_node(const aurora_metrics& m)
{
  json o;
  o["ran_type"] = ran_type_name(m.ran_type);
  AURORA_E3_FIELD(o, m, collection_interval_ms);
  AURORA_E3_FIELD(o, m, seq);
  return o;
}

json encode_cell(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, active_ue_count);
  AURORA_E3_FIELD(o, m, dl_harq_ack);
  AURORA_E3_FIELD(o, m, dl_harq_nack);
  AURORA_E3_FIELD(o, m, dl_harq_total);
  AURORA_E3_FIELD(o, m, dl_harq_loss_rate);
  AURORA_E3_FIELD(o, m, dl_harq_max_cons);
  AURORA_E3_FIELD(o, m, ul_crc_loss);
  AURORA_E3_FIELD(o, m, ul_crc_total);
  AURORA_E3_FIELD(o, m, ul_crc_loss_rate);
  AURORA_E3_FIELD(o, m, sinr_min);
  AURORA_E3_FIELD(o, m, sinr_max);
  AURORA_E3_FIELD(o, m, sinr_avg);
  AURORA_E3_FIELD(o, m, csi_min);
  AURORA_E3_FIELD(o, m, csi_max);
  AURORA_E3_FIELD(o, m, csi_avg);
  AURORA_E3_FIELD(o, m, ri_min);
  AURORA_E3_FIELD(o, m, ri_max);
  AURORA_E3_FIELD(o, m, ri_avg);
  AURORA_E3_FIELD(o, m, ta_min);
  AURORA_E3_FIELD(o, m, ta_max);
  AURORA_E3_FIELD(o, m, ta_avg);
  AURORA_E3_FIELD(o, m, srs_ta_min);
  AURORA_E3_FIELD(o, m, srs_ta_max);
  AURORA_E3_FIELD(o, m, srs_ta_avg);
  AURORA_E3_FIELD(o, m, pucch_ta_min);
  AURORA_E3_FIELD(o, m, pucch_ta_max);
  AURORA_E3_FIELD(o, m, pucch_ta_avg);
  AURORA_E3_FIELD(o, m, bsr_min);
  AURORA_E3_FIELD(o, m, bsr_max);
  AURORA_E3_FIELD(o, m, bsr_avg);
  AURORA_E3_FIELD(o, m, sched_loop_avg_us);
  AURORA_E3_FIELD(o, m, sched_loop_max_us);
  AURORA_E3_FIELD(o, m, dl_prb_utilization_pct);
  AURORA_E3_FIELD(o, m, ul_prb_utilization_pct);
  AURORA_E3_FIELD(o, m, f1u_dl_loss_rate);
  AURORA_E3_FIELD(o, m, ue_attach_count);
  AURORA_E3_FIELD(o, m, ue_detach_count);
  AURORA_E3_FIELD(o, m, du_count);
  return o;
}

json encode_kpm(const aurora_metrics& m)
{
  json     arr = json::array();
  unsigned n   = m.kpm_metric_count < AURORA_MAX_KPM_METRICS ? m.kpm_metric_count : AURORA_MAX_KPM_METRICS;
  for (unsigned i = 0; i != n; ++i) {
    const aurora_kpm_metric_t& k = m.kpm_metrics[i];
    json                       o;
    AURORA_E3_FIELD(o, k, measurement_id);
    o["name"] = c_string(k.name, sizeof(k.name));
    if (k.value_type == 1) {
      o["value"] = k.value_real;
    } else {
      o["value"] = k.value;
    }
    arr.push_back(std::move(o));
  }
  return arr;
}

json encode_rc_cells(const aurora_metrics& m)
{
  json     arr = json::array();
  unsigned n   = m.rc_cell_count < AURORA_MAX_RC_CELLS ? m.rc_cell_count : AURORA_MAX_RC_CELLS;
  for (unsigned i = 0; i != n; ++i) {
    const aurora_rc_stats_t& c = m.rc_metrics[i];
    json                     o;
    AURORA_E3_FIELD(o, c, cell_id);
    AURORA_E3_FIELD(o, c, rsrp_avg);
    AURORA_E3_FIELD(o, c, rsrq_avg);
    AURORA_E3_FIELD(o, c, sinr_avg);
    AURORA_E3_FIELD(o, c, handover_attempts);
    AURORA_E3_FIELD(o, c, handover_success);
    AURORA_E3_FIELD(o, c, handover_success_rate);
    AURORA_E3_FIELD(o, c, cell_load_percent);
    AURORA_E3_FIELD(o, c, interference_level);
    arr.push_back(std::move(o));
  }
  return arr;
}

json encode_fapi(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, fapi_dl_total_pdsch_count);
  AURORA_E3_FIELD(o, m, fapi_dl_total_pdsch_size);
  AURORA_E3_FIELD(o, m, fapi_dl_avg_pdsch);
  AURORA_E3_FIELD(o, m, fapi_ul_total_pusch_count);
  AURORA_E3_FIELD(o, m, fapi_ul_total_pusch_size);
  AURORA_E3_FIELD(o, m, fapi_ul_avg_pusch);
  AURORA_E3_FIELD(o, m, fapi_ue_count);
  AURORA_E3_FIELD(o, m, dl_fapi_mcs_max);
  AURORA_E3_FIELD(o, m, dl_fapi_mcs_min);
  AURORA_E3_FIELD(o, m, dl_fapi_mcs_avg);
  AURORA_E3_FIELD(o, m, dl_fapi_prb_max);
  AURORA_E3_FIELD(o, m, dl_fapi_prb_min);
  AURORA_E3_FIELD(o, m, dl_fapi_prb_avg);
  AURORA_E3_FIELD(o, m, dl_fapi_tbs_max);
  AURORA_E3_FIELD(o, m, dl_fapi_tbs_min);
  AURORA_E3_FIELD(o, m, dl_fapi_tbs_avg);
  AURORA_E3_FIELD(o, m, ul_fapi_mcs_max);
  AURORA_E3_FIELD(o, m, ul_fapi_mcs_min);
  AURORA_E3_FIELD(o, m, ul_fapi_mcs_avg);
  AURORA_E3_FIELD(o, m, ul_fapi_prb_max);
  AURORA_E3_FIELD(o, m, ul_fapi_prb_min);
  AURORA_E3_FIELD(o, m, ul_fapi_prb_avg);
  AURORA_E3_FIELD(o, m, ul_fapi_tbs_max);
  AURORA_E3_FIELD(o, m, ul_fapi_tbs_min);
  AURORA_E3_FIELD(o, m, ul_fapi_tbs_avg);
  return o;
}

json encode_flows(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, bhtx_in_size);
  AURORA_E3_FIELD(o, m, bhtx_out_size);
  AURORA_E3_FIELD(o, m, bhrx_in_size);
  AURORA_E3_FIELD(o, m, bhrx_out_size);
  AURORA_E3_FIELD(o, m, mhtx_in_size);
  AURORA_E3_FIELD(o, m, mhtx_out_size);
  AURORA_E3_FIELD(o, m, mhrx_in_size);
  AURORA_E3_FIELD(o, m, mhrx_out_size);
  AURORA_E3_FIELD(o, m, f1u_rlc_size);
  AURORA_E3_FIELD(o, m, rlc_f1_size);
  AURORA_E3_FIELD(o, m, rlc_mac_size);
  AURORA_E3_FIELD(o, m, mac_rlc_size);
  AURORA_E3_FIELD(o, m, mac_dl_bo);
  return o;
}

json encode_histograms(const aurora_metrics& m)
{
  json o;
  o["bhtx_in"]     = encode_hist(m.hist_bhtx_in);
  o["bhtx_out"]    = encode_hist(m.hist_bhtx_out);
  o["bhrx_in"]     = encode_hist(m.hist_bhrx_in);
  o["bhrx_out"]    = encode_hist(m.hist_bhrx_out);
  o["mhtx_in"]     = encode_hist(m.hist_mhtx_in);
  o["mhtx_out"]    = encode_hist(m.hist_mhtx_out);
  o["mhrx_in"]     = encode_hist(m.hist_mhrx_in);
  o["mhrx_out"]    = encode_hist(m.hist_mhrx_out);
  o["f1u_rlc"]     = encode_hist(m.hist_f1u_rlc);
  o["rlc_f1"]      = encode_hist(m.hist_rlc_f1);
  o["rlc_mac"]     = encode_hist(m.hist_rlc_mac);
  o["mac_rlc"]     = encode_hist(m.hist_mac_rlc);
  o["mac_dl_bo"]   = encode_hist(m.hist_mac_dl_bo);
  o["dl_fapi_mcs"] = encode_hist(m.hist_dl_fapi_mcs);
  o["dl_fapi_prb"] = encode_hist(m.hist_dl_fapi_prb);
  o["ul_fapi_mcs"] = encode_hist(m.hist_ul_fapi_mcs);
  o["ul_fapi_prb"] = encode_hist(m.hist_ul_fapi_prb);
  return o;
}

json encode_ue_mac(const aurora_mac_ue_stats_t& u)
{
  json o;
  AURORA_E3_FIELD(o, u, dl_aggr_tbs);
  AURORA_E3_FIELD(o, u, ul_aggr_tbs);
  AURORA_E3_FIELD(o, u, dl_aggr_bytes_sdus);
  AURORA_E3_FIELD(o, u, ul_aggr_bytes_sdus);
  AURORA_E3_FIELD(o, u, dl_curr_tbs);
  AURORA_E3_FIELD(o, u, ul_curr_tbs);
  AURORA_E3_FIELD(o, u, dl_sched_rb);
  AURORA_E3_FIELD(o, u, ul_sched_rb);
  AURORA_E3_FIELD(o, u, pusch_snr);
  AURORA_E3_FIELD(o, u, pucch_snr);
  AURORA_E3_FIELD(o, u, dl_bler);
  AURORA_E3_FIELD(o, u, ul_bler);
  o["dl_harq"] = u.dl_harq;
  o["ul_harq"] = u.ul_harq;
  AURORA_E3_FIELD(o, u, dl_num_harq);
  AURORA_E3_FIELD(o, u, ul_num_harq);
  AURORA_E3_FIELD(o, u, dl_aggr_prb);
  AURORA_E3_FIELD(o, u, ul_aggr_prb);
  AURORA_E3_FIELD(o, u, dl_aggr_sdus);
  AURORA_E3_FIELD(o, u, ul_aggr_sdus);
  AURORA_E3_FIELD(o, u, dl_aggr_retx_prb);
  AURORA_E3_FIELD(o, u, ul_aggr_retx_prb);
  AURORA_E3_FIELD(o, u, bsr);
  AURORA_E3_FIELD(o, u, frame);
  AURORA_E3_FIELD(o, u, slot);
  AURORA_E3_FIELD(o, u, wb_cqi);
  AURORA_E3_FIELD(o, u, dl_mcs1);
  AURORA_E3_FIELD(o, u, ul_mcs1);
  AURORA_E3_FIELD(o, u, dl_mcs2);
  AURORA_E3_FIELD(o, u, ul_mcs2);
  AURORA_E3_FIELD(o, u, phr);
  AURORA_E3_FIELD(o, u, rsrp_avg);
  AURORA_E3_FIELD(o, u, ri);
  AURORA_E3_FIELD(o, u, pmi_x1);
  AURORA_E3_FIELD(o, u, pmi_x2);
  AURORA_E3_FIELD(o, u, dl_max_mcs);
  AURORA_E3_FIELD(o, u, ssb_rsrp_best);
  AURORA_E3_FIELD(o, u, csirs_rsrp_best);
  AURORA_E3_FIELD(o, u, sinr_csi_x10);
  AURORA_E3_FIELD(o, u, pusch_consec_dtx);
  AURORA_E3_FIELD(o, u, pucch_consec_dtx);
  AURORA_E3_FIELD(o, u, ul_failure);
  AURORA_E3_FIELD(o, u, ul_failure_timer);
  o["dl_harq_rounds"] = u.dl_harq_rounds;
  o["ul_harq_rounds"] = u.ul_harq_rounds;
  AURORA_E3_FIELD(o, u, dl_harq_errors);
  AURORA_E3_FIELD(o, u, ul_harq_errors);
  AURORA_E3_FIELD(o, u, ulsch_dtx);
  AURORA_E3_FIELD(o, u, dl_starvation_events);
  AURORA_E3_FIELD(o, u, ul_starvation_events);
  return o;
}

json encode_ue_fapi(const aurora_fapi_ue_metrics_t& f)
{
  json o;
  AURORA_E3_FIELD(o, f, dl_mcs);
  AURORA_E3_FIELD(o, f, dl_prb);
  AURORA_E3_FIELD(o, f, dl_tbs);
  AURORA_E3_FIELD(o, f, ul_mcs);
  AURORA_E3_FIELD(o, f, ul_prb);
  AURORA_E3_FIELD(o, f, ul_tbs);
  return o;
}

json encode_ue_csi(const aurora_csi_ue_stats_t& c)
{
  json     o;
  unsigned nof_rs = c.nof_rs < AURORA_CSI_MAX_RS ? c.nof_rs : AURORA_CSI_MAX_RS;
  AURORA_E3_FIELD(o, c, nof_reports);
  AURORA_E3_FIELD(o, c, nof_invalid_reports);
  AURORA_E3_FIELD(o, c, rsrp_from_ssb);
  o["cri"]         = json::array();
  o["l1_rsrp_dbm"] = json::array();
  for (unsigned i = 0; i != nof_rs; ++i) {
    o["cri"].push_back(c.cri[i]);
    o["l1_rsrp_dbm"].push_back(c.l1_rsrp_dbm[i]);
  }
  AURORA_E3_FIELD(o, c, l1_rsrp_best_dbm);
  AURORA_E3_FIELD(o, c, ri);
  AURORA_E3_FIELD(o, c, li);
  AURORA_E3_FIELD(o, c, wb_cqi_tb1);
  AURORA_E3_FIELD(o, c, wb_cqi_tb2);
  unsigned nsb1        = c.nof_subbands_tb1 < AURORA_CSI_MAX_SUBBANDS ? c.nof_subbands_tb1 : AURORA_CSI_MAX_SUBBANDS;
  unsigned nsb2        = c.nof_subbands_tb2 < AURORA_CSI_MAX_SUBBANDS ? c.nof_subbands_tb2 : AURORA_CSI_MAX_SUBBANDS;
  o["sb_cqi_diff_tb1"] = std::vector<uint8_t>(c.sb_cqi_diff_tb1, c.sb_cqi_diff_tb1 + nsb1);
  o["sb_cqi_diff_tb2"] = std::vector<uint8_t>(c.sb_cqi_diff_tb2, c.sb_cqi_diff_tb2 + nsb2);
  AURORA_E3_FIELD(o, c, pmi_type);
  AURORA_E3_FIELD(o, c, pmi_two_port);
  AURORA_E3_FIELD(o, c, pmi_i11);
  AURORA_E3_FIELD(o, c, pmi_i12);
  AURORA_E3_FIELD(o, c, pmi_i13);
  AURORA_E3_FIELD(o, c, pmi_i2);
  if (c.pmi_type == AURORA_PMI_TYPEII) {
    unsigned nl = c.t2_nof_layers < AURORA_CSI_MAX_T2_LAYERS ? c.t2_nof_layers : AURORA_CSI_MAX_T2_LAYERS;
    json     t2;
    t2["i12"]    = c.t2_i12;
    t2["layers"] = json::array();
    for (unsigned l = 0; l != nl; ++l) {
      unsigned nc = c.t2_nof_coeffs[l] < AURORA_CSI_MAX_T2_COEFFS ? c.t2_nof_coeffs[l] : AURORA_CSI_MAX_T2_COEFFS;
      json     lo;
      lo["i13"] = c.t2_i13[l];
      lo["i14"] = std::vector<uint8_t>(c.t2_i14[l], c.t2_i14[l] + nc);
      lo["i21"] = std::vector<uint8_t>(c.t2_i21[l], c.t2_i21[l] + nc);
      lo["i22"] = std::vector<uint8_t>(c.t2_i22[l], c.t2_i22[l] + nc);
      t2["layers"].push_back(std::move(lo));
    }
    o["type2"] = std::move(t2);
  }
  return o;
}

json encode_rlc(const aurora_rlc_radio_bearer_stats_t& r)
{
  json o;
  AURORA_E3_FIELD(o, r, rbid);
  AURORA_E3_FIELD(o, r, mode);
  AURORA_E3_FIELD(o, r, txpdu_pkts);
  AURORA_E3_FIELD(o, r, txpdu_bytes);
  AURORA_E3_FIELD(o, r, txsdu_pkts);
  AURORA_E3_FIELD(o, r, txsdu_bytes);
  AURORA_E3_FIELD(o, r, rxpdu_pkts);
  AURORA_E3_FIELD(o, r, rxpdu_bytes);
  AURORA_E3_FIELD(o, r, rxsdu_pkts);
  AURORA_E3_FIELD(o, r, rxsdu_bytes);
  AURORA_E3_FIELD(o, r, rxpdu_lost_pkts);
  AURORA_E3_FIELD(o, r, rxpdu_dd_pkts);
  AURORA_E3_FIELD(o, r, rxpdu_ow_pkts);
  AURORA_E3_FIELD(o, r, txsdu_wt_us);
  AURORA_E3_FIELD(o, r, rxsdu_wt_us);
  AURORA_E3_FIELD(o, r, txpdu_retx_pkts);
  AURORA_E3_FIELD(o, r, txpdu_retx_bytes);
  AURORA_E3_FIELD(o, r, txpdu_dd_pkts);
  AURORA_E3_FIELD(o, r, txpdu_dd_bytes);
  AURORA_E3_FIELD(o, r, txpdu_segmented);
  AURORA_E3_FIELD(o, r, txbuf_occ_bytes);
  AURORA_E3_FIELD(o, r, rxbuf_occ_bytes);
  AURORA_E3_FIELD(o, r, retransmit_list_depth);
  AURORA_E3_FIELD(o, r, tx_window_stall_events);
  AURORA_E3_FIELD(o, r, pdcp_rlc_q_depth);
  AURORA_E3_FIELD(o, r, rlc_pdcp_q_depth);
  return o;
}

json encode_pdcp(const aurora_pdcp_radio_bearer_stats_t& p)
{
  json o;
  AURORA_E3_FIELD(o, p, rbid);
  AURORA_E3_FIELD(o, p, mode);
  AURORA_E3_FIELD(o, p, txpdu_pkts);
  AURORA_E3_FIELD(o, p, txpdu_bytes);
  AURORA_E3_FIELD(o, p, txsdu_pkts);
  AURORA_E3_FIELD(o, p, txsdu_bytes);
  AURORA_E3_FIELD(o, p, rxpdu_pkts);
  AURORA_E3_FIELD(o, p, rxpdu_bytes);
  AURORA_E3_FIELD(o, p, rxsdu_pkts);
  AURORA_E3_FIELD(o, p, rxsdu_bytes);
  AURORA_E3_FIELD(o, p, rxpdu_lost_pkts);
  AURORA_E3_FIELD(o, p, rxpdu_dd_pkts);
  AURORA_E3_FIELD(o, p, rxpdu_oo_pkts);
  AURORA_E3_FIELD(o, p, tx_proc_latency_us);
  return o;
}

json encode_gtp(const aurora_gtp_tunnel_stats_t& g)
{
  json o;
  AURORA_E3_FIELD(o, g, teidgnb);
  AURORA_E3_FIELD(o, g, qfi);
  AURORA_E3_FIELD(o, g, teidupf);
  AURORA_E3_FIELD(o, g, tx_pkts);
  AURORA_E3_FIELD(o, g, tx_bytes);
  AURORA_E3_FIELD(o, g, tx_gpdupkts);
  AURORA_E3_FIELD(o, g, tx_err_pkts);
  AURORA_E3_FIELD(o, g, rx_pkts);
  AURORA_E3_FIELD(o, g, rx_bytes);
  AURORA_E3_FIELD(o, g, rx_gpdupkts);
  AURORA_E3_FIELD(o, g, rx_err_pkts);
  AURORA_E3_FIELD(o, g, rx_unkwn_teid_pkts);
  AURORA_E3_FIELD(o, g, tx_proc_latency_us);
  AURORA_E3_FIELD(o, g, tx_buf_occ);
  AURORA_E3_FIELD(o, g, rx_buf_occ);
  AURORA_E3_FIELD(o, g, tx_loss_rate);
  AURORA_E3_FIELD(o, g, rx_loss_rate);
  return o;
}

json encode_f1c_proc(const aurora_f1c_proc_stats_t& p)
{
  json o;
  AURORA_E3_FIELD(o, p, attempts);
  AURORA_E3_FIELD(o, p, success);
  AURORA_E3_FIELD(o, p, failure);
  AURORA_E3_FIELD(o, p, latency_max_us);
  AURORA_E3_FIELD(o, p, latency_avg_us);
  return o;
}

json encode_f1c(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, f1c_dl_msgs);
  AURORA_E3_FIELD(o, m, f1c_ul_msgs);
  AURORA_E3_FIELD(o, m, f1c_dl_bytes);
  AURORA_E3_FIELD(o, m, f1c_ul_bytes);
  AURORA_E3_FIELD(o, m, f1c_dl_bytes_max);
  AURORA_E3_FIELD(o, m, f1c_ul_bytes_max);
  AURORA_E3_FIELD(o, m, f1c_dl_rrc_msgs);
  AURORA_E3_FIELD(o, m, f1c_ul_rrc_msgs);
  AURORA_E3_FIELD(o, m, f1c_dl_rrc_bytes);
  AURORA_E3_FIELD(o, m, f1c_ul_rrc_bytes);
  AURORA_E3_FIELD(o, m, f1c_decode_errors);
  AURORA_E3_FIELD(o, m, f1c_init_ul_rrc);
  AURORA_E3_FIELD(o, m, f1c_paging);
  AURORA_E3_FIELD(o, m, f1c_ue_ctx_release_req);
  AURORA_E3_FIELD(o, m, f1c_resets);
  AURORA_E3_FIELD(o, m, f1c_error_indications);
  AURORA_E3_FIELD(o, m, f1c_proc_timeouts);
  AURORA_E3_FIELD(o, m, f1c_links_up);
  o["f1_setup"]       = encode_f1c_proc(m.f1c_f1_setup);
  o["ue_ctx_setup"]   = encode_f1c_proc(m.f1c_ue_ctx_setup);
  o["ue_ctx_mod"]     = encode_f1c_proc(m.f1c_ue_ctx_mod);
  o["ue_ctx_release"] = encode_f1c_proc(m.f1c_ue_ctx_release);
  return o;
}

json encode_f1u(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, f1u_dl_pdus);
  AURORA_E3_FIELD(o, m, f1u_ul_pdus);
  AURORA_E3_FIELD(o, m, f1u_dl_dropped);
  AURORA_E3_FIELD(o, m, f1u_ul_dropped);
  AURORA_E3_FIELD(o, m, f1u_dl_discards);
  AURORA_E3_FIELD(o, m, f1u_dds);
  AURORA_E3_FIELD(o, m, f1u_dds_failures);
  return o;
}

json encode_handover(const aurora_metrics_ext& x)
{
  json o;
  o["du"]         = json::array();
  unsigned nof_du = x.ho_du_count < AURORA_MAX_HO_DUS ? x.ho_du_count : AURORA_MAX_HO_DUS;
  for (unsigned i = 0; i != nof_du; ++i) {
    const aurora_du_ho_stats_t& d = x.ho_du[i];
    if (d.valid == 0) {
      continue;
    }
    json e;
    AURORA_E3_FIELD(e, d, gnb_du_id);
    AURORA_E3_FIELD(e, d, exec_attempts);
    AURORA_E3_FIELD(e, d, exec_success);
    AURORA_E3_FIELD(e, d, exec_attempts_total);
    AURORA_E3_FIELD(e, d, exec_success_total);
    AURORA_E3_FIELD(e, d, exec_success_rate);
    o["du"].push_back(std::move(e));
  }
  AURORA_E3_FIELD(o, x, ho_prep_attempts);
  AURORA_E3_FIELD(o, x, ho_prep_success);
  AURORA_E3_FIELD(o, x, ho_prep_attempts_total);
  AURORA_E3_FIELD(o, x, ho_prep_success_total);
  return o;
}

json encode_process(const aurora_metrics& m)
{
  json o;
  AURORA_E3_FIELD(o, m, ru_utime_us);
  AURORA_E3_FIELD(o, m, ru_stime_us);
  AURORA_E3_FIELD(o, m, ru_maxrss_kb);
  AURORA_E3_FIELD(o, m, ru_minflt);
  AURORA_E3_FIELD(o, m, ru_majflt);
  AURORA_E3_FIELD(o, m, ru_inblock);
  AURORA_E3_FIELD(o, m, ru_oublock);
  AURORA_E3_FIELD(o, m, ru_nvcsw);
  AURORA_E3_FIELD(o, m, ru_nivcsw);
  return o;
}

json encode_threads(const aurora_thread_stats_t& ts)
{
  json arr = json::array();
  for (unsigned i = 0; i != AURORA_MAX_MONITORED_THREADS; ++i) {
    const aurora_thread_metrics_t& t = ts.threads[i];
    if (t.valid == 0) {
      continue;
    }
    json o;
    o["process_name"] = c_string(t.process_name, sizeof(t.process_name));
    o["thread_name"]  = c_string(t.thread_name, sizeof(t.thread_name));
    AURORA_E3_FIELD(o, t, pid);
    AURORA_E3_FIELD(o, t, tid);
    AURORA_E3_FIELD(o, t, cpu_id);
    AURORA_E3_FIELD(o, t, total_events);
    AURORA_E3_FIELD(o, t, max_runtime_ns);
    o["hist_runtime"]          = t.hist_runtime.bin;
    o["hist_runtime_overflow"] = t.hist_runtime.overflow;
    AURORA_E3_FIELD(o, t, total_runtime_ns);
    AURORA_E3_FIELD(o, t, mean_ns);
    AURORA_E3_FIELD(o, t, range_ns);
    AURORA_E3_FIELD(o, t, variance_ns2);
    AURORA_E3_FIELD(o, t, std_deviation_ns);
    AURORA_E3_FIELD(o, t, skewness);
    AURORA_E3_FIELD(o, t, kurtosis);
    AURORA_E3_FIELD(o, t, outliers_low);
    AURORA_E3_FIELD(o, t, outliers_high);
    AURORA_E3_FIELD(o, t, irq_count);
    AURORA_E3_FIELD(o, t, voluntary_ctxsw);
    AURORA_E3_FIELD(o, t, involuntary_ctxsw);
    AURORA_E3_FIELD(o, t, cpu_migrations);
    arr.push_back(std::move(o));
  }
  return arr;
}

/// Per-bearer arrays of one UE slot; entries with rnti == 0 are unused.
template <typename T, typename Encoder>
json encode_bearers(const T (&rbs)[MAX_RBS_PER_UE], Encoder enc)
{
  json arr = json::array();
  for (const T& rb : rbs) {
    if (rb.rnti != 0) {
      arr.push_back(enc(rb));
    }
  }
  return arr;
}

json encode_ues(const aurora_metrics& m, const e3sm::telemetry_mask& mask)
{
  using e3sm::telemetry;
  const bool mac  = e3sm::has(mask, telemetry::ue_mac);
  const bool fapi = e3sm::has(mask, telemetry::ue_fapi);
  const bool csi  = e3sm::has(mask, telemetry::ue_csi);
  const bool rlc  = e3sm::has(mask, telemetry::ue_rlc);
  const bool pdcp = e3sm::has(mask, telemetry::ue_pdcp);
  const bool gtp  = e3sm::has(mask, telemetry::ue_gtp);

  json arr = json::array();
  for (unsigned i = 0; i != MAX_NO_UEs; ++i) {
    json ue;
    // The UE key of a slot is the same in every per-UE array of the slot (C-RNTI, or UE index + 1 where the layer
    // has no RNTI, see aurora_shm.h). Take it from the first array that holds the slot.
    uint32_t rnti = 0;
    if (mac && m.mac_ue_stats[i].rnti != 0) {
      ue["mac"] = encode_ue_mac(m.mac_ue_stats[i]);
      rnti      = m.mac_ue_stats[i].rnti;
    }
    if (fapi && m.fapi_ue_metrics[i].rnti != 0) {
      ue["fapi"] = encode_ue_fapi(m.fapi_ue_metrics[i]);
      rnti       = rnti != 0 ? rnti : m.fapi_ue_metrics[i].rnti;
    }
    if (csi && m.ext.csi[i].rnti != 0) {
      ue["csi"] = encode_ue_csi(m.ext.csi[i]);
      rnti      = rnti != 0 ? rnti : m.ext.csi[i].rnti;
    }
    if (rlc) {
      json rbs = encode_bearers(m.rlc_rb_stats[i], encode_rlc);
      if (!rbs.empty()) {
        ue["rlc"] = std::move(rbs);
      }
    }
    if (pdcp) {
      json rbs = encode_bearers(m.pdcp_rb_stats[i], encode_pdcp);
      if (!rbs.empty()) {
        ue["pdcp"] = std::move(rbs);
      }
    }
    if (gtp) {
      json tun = encode_bearers(m.gtp_tunnel_stats[i], encode_gtp);
      if (!tun.empty()) {
        ue["gtp"] = std::move(tun);
      }
    }
    if (ue.empty()) {
      continue;
    }
    if (rnti == 0) {
      // Only bearer arrays matched: take the key of the first valid bearer/tunnel.
      for (unsigned rb = 0; rb != MAX_RBS_PER_UE && rnti == 0; ++rb) {
        rnti = m.rlc_rb_stats[i][rb].rnti != 0    ? m.rlc_rb_stats[i][rb].rnti
               : m.pdcp_rb_stats[i][rb].rnti != 0 ? m.pdcp_rb_stats[i][rb].rnti
                                                  : m.gtp_tunnel_stats[i][rb].rnti;
      }
    }
    ue["ue_slot"] = i;
    ue["rnti"]    = rnti;
    arr.push_back(std::move(ue));
  }
  return arr;
}

} // namespace

json e3sm::ran_function_data()
{
  json arr = json::array();
  for (const stream_desc& s : streams) {
    json d;
    d["telemetryIdentifier"] = static_cast<uint32_t>(s.id);
    d["stream_id"]           = s.stream_id;
    d["data_type"]           = s.data_type;
    d["description"]         = s.description;
    d["status"]              = "available";
    json delivery;
    delivery["transport_type"] = "protocolData";
    delivery["keyword"]        = s.stream_id;
    delivery["encoding"]       = "json";
    d["delivery_method"]       = std::move(delivery);
    arr.push_back(std::move(d));
  }
  return arr;
}

json e3sm::ran_function_definition()
{
  json ids = json::array();
  for (uint32_t id = 1; id <= nof_telemetry_ids; ++id) {
    ids.push_back(id);
  }
  json rf;
  rf["ranFunctionIdentifier"]   = ran_function_id;
  rf["ranFunctionName"]         = ran_function_name;
  rf["ranFunctionVersion"]      = ran_function_version;
  rf["telemetryIdentifierList"] = std::move(ids);
  rf["controlIdentifierList"]   = json::array();
  rf["ranFunctionData"]         = ran_function_data();
  return rf;
}

json e3sm::encode_protocol_data(const aurora_metrics& m, const telemetry_mask& mask)
{
  json pd = json::object();
  if (has(mask, telemetry::timestamp)) {
    pd["timestamp_us"] = m.timestamp_us;
  }
  if (has(mask, telemetry::node)) {
    pd["node"] = encode_node(m);
  }
  if (has(mask, telemetry::anomaly_label)) {
    pd["anomaly_label"] = c_string(m.ext.anomaly_label, sizeof(m.ext.anomaly_label));
  }
  if (has(mask, telemetry::cell)) {
    pd["cell"] = encode_cell(m);
  }
  if (has(mask, telemetry::kpm)) {
    pd["kpm"] = encode_kpm(m);
  }
  if (has(mask, telemetry::rc_cells)) {
    pd["rc_cells"] = encode_rc_cells(m);
  }
  if (has(mask, telemetry::fapi)) {
    pd["fapi"] = encode_fapi(m);
  }
  if (has(mask, telemetry::flows)) {
    pd["flows"] = encode_flows(m);
  }
  if (has(mask, telemetry::histograms)) {
    pd["hist"] = encode_histograms(m);
  }
  if (has(mask, telemetry::ue_mac) || has(mask, telemetry::ue_fapi) || has(mask, telemetry::ue_csi) ||
      has(mask, telemetry::ue_rlc) || has(mask, telemetry::ue_pdcp) || has(mask, telemetry::ue_gtp)) {
    pd["ues"] = encode_ues(m, mask);
  }
  if (has(mask, telemetry::f1c)) {
    pd["f1c"] = encode_f1c(m);
  }
  if (has(mask, telemetry::f1u)) {
    pd["f1u"] = encode_f1u(m);
  }
  if (has(mask, telemetry::handover)) {
    pd["handover"] = encode_handover(m.ext);
  }
  if (has(mask, telemetry::process)) {
    pd["process"] = encode_process(m);
  }
  if (has(mask, telemetry::threads)) {
    pd["threads"] = encode_threads(m.thread_stats);
  }
  return pd;
}
