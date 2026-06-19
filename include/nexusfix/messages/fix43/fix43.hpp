#pragma once

// FIX 4.3 Application Layer Messages

#include "nexusfix/messages/fix43/new_order_single.hpp"
#include "nexusfix/messages/fix43/order_cancel_request.hpp"
#include "nexusfix/messages/fix43/execution_report.hpp"
#include "nexusfix/messages/fix43/market_data_request.hpp"
#include "nexusfix/messages/fix43/market_data_snapshot.hpp"

namespace nfx::fix43 {

// All FIX 4.3 application message types:
// - NewOrderSingle (D) - new_order_single.hpp
// - OrderCancelRequest (F) - order_cancel_request.hpp
// - ExecutionReport (8) - execution_report.hpp
// - MarketDataRequest (V) - market_data_request.hpp (new in 4.3)
// - MarketDataSnapshotFullRefresh (W) - market_data_snapshot.hpp (new in 4.3)
//
// FIX 4.3 differences from FIX 4.2:
// - MarketData messages introduced (V, W, X)
// - ExecTransType (Tag 20) still required in ExecutionReport
// - LeavesQty (Tag 151) promoted to required in ExecutionReport
// - Product (Tag 460) introduced as optional field
//
// FIX 4.3 differences from FIX 4.4:
// - ExecTransType (Tag 20) still present (removed in 4.4)
// - HandlInst (Tag 21) still required in NewOrderSingle (optional in 4.4)
//
// Session messages (Logon, Logout, Heartbeat, etc.) reuse the fix44
// namespace with begin_string set to "FIX.4.3".

} // namespace nfx::fix43
