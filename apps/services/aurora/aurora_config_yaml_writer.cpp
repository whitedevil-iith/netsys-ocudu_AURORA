#include "aurora_config_yaml_writer.h"
#include "aurora_appconfig.h"
#include "ocudu/adt/format.h"
#include "ocudu/adt/span.h"

using namespace ocudu;

void app_services::fill_aurora_config_in_yaml_schema(YAML::Node& node, const aurora_appconfig& config)
{
  YAML::Node sub                    = node["aurora"];
  sub["enable"]                     = config.enabled;
  sub["collection_interval_ms"]     = config.collection_interval.count();
  sub["shm_key"]                    = config.shm_key;
  sub["ran_type"]                   = config.ran_type;
  sub["thread_metrics_enabled"]     = config.thread_metrics_enabled;
  sub["thread_metrics_backend"]     = config.thread_metrics_backend;
  sub["thread_metrics_whitelist"]   = config.thread_metrics_whitelist;
  sub["anomaly_enabled"]            = config.anomaly_enabled;
  sub["e3_enable"]                  = config.e3_enabled;
  sub["e3_bind_addr"]               = config.e3_bind_address;
  sub["e3_rep_port"]                = config.e3_rep_port;
  sub["e3_pub_port"]                = config.e3_pub_port;
  sub["e3_sub_port"]                = config.e3_sub_port;
  if (config.cpus.any()) {
    sub["cpus"] = fmt::format("{:n}", span<const size_t>(config.cpus.get_cpu_ids()));
  }
}
