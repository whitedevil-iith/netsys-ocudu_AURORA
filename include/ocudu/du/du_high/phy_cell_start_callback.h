// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/du_types.h"
#include "ocudu/support/async/async_task.h"

namespace ocudu {
namespace odu {

/// Callback to run PHY cell start (e.g. FAPI P5 procedure) before starting the MAC cell.
/// Uses an interface so it can return move-only async_task<bool> from type-erased storage.
class phy_cell_start_callback
{
public:
  virtual ~phy_cell_start_callback() = default;

  virtual async_task<bool> run(du_cell_index_t cell_index) = 0;
};

} // namespace odu
} // namespace ocudu
