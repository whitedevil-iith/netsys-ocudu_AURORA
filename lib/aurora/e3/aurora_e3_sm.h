// AURORA E3 service model ("AURORA_KPM").
//
// Describes how the AURORA shared-memory snapshot (struct aurora_metrics) is exposed over the E3 interface, the
// dApp-facing interface used by NVIDIA's aerial-sample-apps E3 Manager. It follows the same pattern as the cuBB
// E3 agent (cuPHY-CP/data_lake/e3_agent.*), which exposes "NVIDIA KPM" as RAN function 2:
//
//  - One RAN function, advertised in the E3 Setup Response (ranFunctionList[]). The E3 Manager keeps a single
//    subscription per agent, so all AURORA telemetry lives in one RAN function and a dApp selects what it wants with
//    the telemetry identifiers of that one subscription.
//  - Telemetry identifiers are stable wire values. Each one selects a group of fields (a "stream") of the AURORA
//    snapshot, not a single field: the snapshot has several hundred fields.
//  - Every stream is delivered inline in the Indication Message (delivery_method.transport_type = "protocolData",
//    JSON). No shared memory region is advertised: one full snapshot is small at the AURORA collection interval.
//
// Indication payload (protocolData) layout:
//   root scalars/objects : timestamp_us, node, anomaly_label, cell, kpm[], rc_cells[], fapi, flows, hist,
//                          f1c, f1u, handover, process, threads[]
//   ues[]                : one entry per active UE slot ({"ue_slot", "rnti", ...}), holding the per-UE streams
//                          mac, fapi, csi, rlc[], pdcp[], gtp[]
// Only the subscribed streams are present.
//
// DO NOT reorder or reuse telemetry identifiers. New streams go at the end only.

#pragma once

#include "aurora_shm.h"
#include "nlohmann/json.hpp"
#include <bitset>
#include <cstdint>
#include <string_view>

namespace ocudu {
namespace aurora {
namespace e3sm {

/// E3AP protocol version implemented by the AURORA agent (same as the cuBB agent and the E3 Manager).
constexpr std::string_view e3ap_protocol_version = "1.0.0";

/// RAN function identifier of the AURORA service model. NVIDIA KPM uses 2; AURORA uses a distinct value so that a
/// dApp connected to both agents can tell the indications apart.
constexpr uint32_t         ran_function_id      = 100;
constexpr std::string_view ran_function_name    = "AURORA_KPM";
constexpr std::string_view ran_function_version = "1.0.0";

/// Telemetry identifiers (E3-SubscriptionRequest telemetryIdentifierList). Wire values, append only.
enum class telemetry : uint32_t {
  timestamp     = 1,  ///< root timestamp_us: wall clock (us since epoch) of the snapshot.
  node          = 2,  ///< root node{}: ran_type, collection_interval_ms, seq.
  anomaly_label = 3,  ///< root anomaly_label: label set by the anomaly-injector sidecar.
  cell          = 4,  ///< root cell{}: cell-level KPIs (HARQ/CRC, SINR/CQI/RI/TA/BSR min-max-avg, PRB use, churn).
  kpm           = 5,  ///< root kpm[]: O-RAN KPM measurements (id, name, value).
  rc_cells      = 6,  ///< root rc_cells[]: radio-control cell stats (RSRP, SINR, load, interference, HO).
  fapi          = 7,  ///< root fapi{}: FAPI PDSCH/PUSCH counts and MCS/PRB/TBS min-max-avg.
  flows         = 8,  ///< root flows{}: backhaul/midhaul/F1-U/RLC/MAC byte counters and MAC DL buffer occupancy.
  histograms    = 9,  ///< root hist{}: packet-size and FAPI MCS/PRB histograms.
  ue_mac        = 10, ///< ues[].mac{}: per-UE MAC scheduler stats.
  ue_fapi       = 11, ///< ues[].fapi{}: per-UE last DL/UL FAPI PDU parameters.
  ue_csi        = 12, ///< ues[].csi{}: per-UE content of the last CSI report.
  ue_rlc        = 13, ///< ues[].rlc[]: per-bearer RLC stats.
  ue_pdcp       = 14, ///< ues[].pdcp[]: per-bearer PDCP stats.
  ue_gtp        = 15, ///< ues[].gtp[]: per-tunnel GTP-U stats.
  f1c           = 16, ///< root f1c{}: F1AP message counters and procedure stats.
  f1u           = 17, ///< root f1u{}: NR-U PDU counters.
  handover      = 18, ///< root handover{}: HO executions per source DU and HO preparations.
  process       = 19, ///< root process{}: getrusage() of the RAN process.
  threads       = 20, ///< root threads[]: per-thread runtime distribution metrics.
};

/// Number of telemetry identifiers (1..nof_telemetry_ids are valid).
constexpr uint32_t nof_telemetry_ids = 20;

/// Set of subscribed streams; bit (id - 1) is set for telemetry identifier id.
using telemetry_mask = std::bitset<nof_telemetry_ids>;

/// True if \p id is a valid AURORA telemetry identifier.
constexpr bool is_valid_telemetry_id(uint32_t id)
{
  return id >= 1 && id <= nof_telemetry_ids;
}

inline bool has(const telemetry_mask& mask, telemetry t)
{
  return mask.test(static_cast<uint32_t>(t) - 1);
}

inline void add(telemetry_mask& mask, telemetry t)
{
  mask.set(static_cast<uint32_t>(t) - 1);
}

/// Stream descriptors for the E3 Setup Response (ranFunctionList[].ranFunctionData), one per telemetry identifier.
nlohmann::json ran_function_data();

/// Full ranFunctionList[] entry of the AURORA service model.
nlohmann::json ran_function_definition();

/// Builds the Indication Message protocolData for the streams in \p mask from the snapshot \p m.
nlohmann::json encode_protocol_data(const aurora_metrics& m, const telemetry_mask& mask);

} // namespace e3sm
} // namespace aurora
} // namespace ocudu
