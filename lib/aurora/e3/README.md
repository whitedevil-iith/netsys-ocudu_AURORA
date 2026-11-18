# AURORA over E3: the `AURORA_KPM` service model

Each OCUDU process (`gnb`, `odu`, `ocu`, `ocucp`, `ocuup`) can run an E3 agent. The agent serves the AURORA snapshot
to dApps over the E3 interface used by the NVIDIA dApp framework
([aerial-sample-apps](https://github.com/NVIDIA/aerial-sample-apps), `dapps/common/e3_manager`).

A dApp can then use AURORA L2/L3 metrics next to, or instead of, the NVIDIA L1 (cuBB) telemetry. The E3 Manager
connects to each agent as one entry of its `e3_agents` list.

| | cuBB E3 agent (`NVIDIA_L1`) | AURORA E3 agent (`OCUDU_<ROLE>`) |
|---|---|---|
| Source | `cuPHY-CP/data_lake/e3_agent.*` | `lib/aurora/e3/aurora_e3_agent.*` |
| RAN function | `2` (NVIDIA KPM) | `100` (`AURORA_KPM`) |
| Telemetry ID granularity | one field per ID (79 IDs) | one field group (stream) per ID (20 IDs) |
| Indication trigger | each PUSCH / SRS slot | each AURORA collection interval (`collection_interval_ms`) |
| Time reference | `sfn`, `slot`, `timestamp` (ns) | `timestamp_us` (wall clock, us) |
| Bulk data | POSIX shm `/e3_ran_buffers` (IQ, H estimates) | none: everything is inline JSON |
| Control | none (negative `messageAck`) | none (negative `messageAck`) |

## Enabling

```yaml
aurora:
  enable: true
  collection_interval_ms: 100
  e3_enable: true
  e3_bind_addr: 127.0.0.1   # "*" to accept an E3 Manager on another host / pod
  e3_rep_port: 9990         # setup (REQ/REP)
  e3_pub_port: 9991         # indications and responses (agent PUB)
  e3_sub_port: 9999         # subscription / control / release (agent SUB)
```

- The agent is built when ZeroMQ is found (`AURORA: ZeroMQ found - E3 agent enabled` in the CMake output).
  - Without ZeroMQ, `e3_enable: true` only logs a warning.
- The default ports are the ones of the `OAI_L2` entry of the sample `e3_config.json`.
  - Like `shm_key`, use different ports for every process on a host.
- If the sockets cannot be bound, AURORA logs an error and keeps running without the agent.

## Transport and procedures

The socket roles are the same as in the cuBB agent. The agent binds all three sockets, and the E3 Manager connects.

| Socket | Direction | Messages |
|---|---|---|
| REP `e3_rep_port` | Manager REQ to agent | `setupRequest` / `setupResponse` |
| PUB `e3_pub_port` | agent to Manager SUB | `subscriptionResponse`, `indicationMessage`, `releaseMessage`, `messageAck` |
| SUB `e3_sub_port` | Manager PUB to agent | `subscriptionRequest`, `subscriptionDelete`, `dAppControlAction`, `releaseMessage` |

- **Setup.**
  - `e3apProtocolVersion` must be `1.0.0`.
  - The response assigns the `dAppIdentifier` and sets `ranIdentifier` to `OCUDU_GNB`, `OCUDU_DU`, `OCUDU_CU`,
    `OCUDU_CU_CP` or `OCUDU_CU_UP`.
  - It lists one RAN function: id `100`, `ranFunctionName` `AURORA_KPM`, `telemetryIdentifierList` 1..20,
    `ranFunctionData` with one stream descriptor per ID.
- **Subscription.**
  - `ranFunctionIdentifier` must be `100`.
  - `telemetryIdentifierList` must be non-empty, with IDs in 1..20.
  - `periodicity` is in us and defaults to the collection interval.
  - `subscriptionTime` is in s; `0` means no expiry.
  - Requests for dApps set up through another agent are ignored, as in the cuBB agent.
- **Indication.**
  - Sent from the AURORA publisher thread right after an interval has been written to the SysV segment. It carries
    exactly the snapshot that `aurora_reader` sees.
  - The periodicity is rounded to the collection interval. An interval is sent when at least
    `periodicity - interval/2` has passed since the previous one.
- **Release.**
  - A Manager `releaseMessage` drops the dApp and its subscriptions.
  - The agent also releases dApps that have had no subscription and no message for 30 min.
  - On shutdown, the agent sends a `releaseMessage` for every dApp.

Sends never block: a missing or slow E3 Manager drops indications (PUB, `ZMQ_DONTWAIT`).

The agent thread (`aurora_e3`) and the publisher run on the AURORA CPUs (`aurora.cpus`). Nothing runs on the RAN
threads.

## Telemetry identifiers

The identifiers are wire values. Never reorder or reuse one; add new streams at the end
(`lib/aurora/e3/aurora_e3_sm.h`).

| ID | Stream (`protocolData` key) | Content (fields keep their `aurora_shm.h` names) |
|---:|---|---|
| 1 | `timestamp_us` | Snapshot wall clock, us since epoch |
| 2 | `node` | `ran_type` (`gnb`/`du`/`cu`/`cu_cp`/`cu_up`), `collection_interval_ms`, `seq` |
| 3 | `anomaly_label` | Label set by the anomaly-injector sidecar (`normal` until the first one) |
| 4 | `cell` | `active_ue_count`, DL HARQ and UL CRC loss, SINR/CQI/RI/TA/SRS-TA/PUCCH-TA/BSR min/max/avg, scheduler loop time, DL/UL PRB utilisation, F1-U DL loss rate, UE attach/detach, `du_count` |
| 5 | `kpm[]` | `{measurement_id, name, value}` O-RAN KPM measurements |
| 6 | `rc_cells[]` | Per cell: RSRP, RSRQ, SINR, HO attempts/success/rate, load %, interference level |
| 7 | `fapi` | PDSCH/PUSCH counts and sizes, MCS/PRB/TBS min/max/avg |
| 8 | `flows` | `bh*`/`mh*` in/out, `f1u_rlc`, `rlc_f1`, `rlc_mac`, `mac_rlc` byte counters, `mac_dl_bo` |
| 9 | `hist` | 16-bin histograms `{bins, count, min_val, max_val, sum}` of the flows and FAPI MCS/PRB |
| 10 | `ues[].mac` | Per-UE MAC scheduler stats (`mac_ue_stats`) |
| 11 | `ues[].fapi` | Per-UE last DL/UL MCS, PRB, TBS |
| 12 | `ues[].csi` | Per-UE last CSI report: CRI/SSBRI and L1-RSRP, RI, LI, wideband and subband CQI, PMI (Type-I, Type-II in `type2`) |
| 13 | `ues[].rlc[]` | Per-bearer RLC stats (`rbid`, `mode`, PDU/SDU counters, ARQ, buffer occupancy, queue depths) |
| 14 | `ues[].pdcp[]` | Per-bearer PDCP stats |
| 15 | `ues[].gtp[]` | Per-tunnel GTP-U stats (TEIDs, QFI, counters, loss rates) |
| 16 | `f1c` | F1AP message, byte and RRC-transfer counters, error counters, `f1_setup`/`ue_ctx_setup`/`ue_ctx_mod`/`ue_ctx_release` procedure stats |
| 17 | `f1u` | NR-U PDU, drop, discard and delivery-status counters |
| 18 | `handover` | `du[]` executions per source DU and HO preparations |
| 19 | `process` | `getrusage()` of the RAN process |
| 20 | `threads[]` | Per-thread runtime distribution (only valid slots) |

**Per-UE layout.**
- `ues[]` holds one entry per active UE slot: `{"ue_slot": <0..99>, "rnti": <key>, "mac": {...}, "csi": {...},
  "rlc": [...], ...}`. It carries only the subscribed per-UE streams, and only the slots that have data in them.
- `rnti` is the key used by AURORA. That is the C-RNTI where the layer knows it, otherwise UE index + 1 (CU-UP, see
  `aurora_shm.h`). Join the per-UE data of the DU and the CU-UP on `ue_slot` only when both use the same key.
- Unused bearer, tunnel and thread slots are left out.

**Units and semantics** are those of the SysV snapshot: interval deltas or gauges, as documented in `aurora_shm.h`
and in `../README.md`.

Example indication (IDs 1, 3, 4, 10):

```json
{"type":"indicationMessage","id":57,"dAppIdentifier":1,"ranFunctionIdentifier":100,"subscriptionId":1,
 "protocolData":{"timestamp_us":1795000000123456,"anomaly_label":"normal",
   "cell":{"active_ue_count":1,"sinr_avg":18.0,"ul_crc_loss_rate":0.0,"...":"..."},
   "ues":[{"ue_slot":0,"rnti":17921,"mac":{"wb_cqi":12,"pusch_snr":18.0,"...":"..."}}]}}
```

## E3 Manager configuration

Add the agent to `e3_manager.e3_agents` in the dApp `e3_config.json` (see `e3_config.aurora.json`).

```json
{
  "name": "OCUDU_DU",
  "enabled": true,
  "host": "127.0.0.1",
  "agent_rep_port": 9990,
  "agent_pub_port": 9991,
  "agent_sub_port": 9999,
  "auto_setup": true,
  "subscription_options": {
    "auto_subscribe": true,
    "telemetry_ids": [1, 3, 4, 10, 12],
    "control_ids": [],
    "ran_function_id": 100,
    "periodicity_us": 100000,
    "subscription_time_s": 0
  }
}
```

- **AURORA only (no cuBB).** Set `deployment.shared_memory.required` to `false`: there is no `/e3_ran_buffers`
  segment, and AURORA does not need `ipc_mode`.
- **Together with `NVIDIA_L1`.**
  - Keep both agents. The E3 Manager holds one subscription per agent, and the handler gets `ctx.agent_name` to tell
    them apart.
  - Align L1 and L2 data by time: cuBB indications are per slot (`timestamp` ns, `sfn`/`slot`); AURORA indications are
    per interval (`timestamp_us`).
- **Kubernetes.** A dApp sidecar in the DU pod reaches the agent on `127.0.0.1`. From another pod, set
  `e3_bind_addr: "*"` and use the pod address. E3 has no authentication, so keep the ports off untrusted networks.

## Tests

- `tests/unittests/aurora/aurora_e3_test` checks two things:
  - the service-model encoding;
  - a full E3AP exchange against a running AURORA instance (setup, version check, rejected RAN function,
    subscription, periodic indications, delete, release), with the test acting as the E3 Manager.
- Interoperability was also checked against the unmodified aerial-sample-apps `E3Manager` (`e3_manager.cpp`), with
  `ran_function_id: 100`. Setup, auto-subscription and indications at the 100 ms interval all worked.
