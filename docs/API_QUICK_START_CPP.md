# NexusFIX C++ Quick Start

A self-contained example that runs a FIX server and client on localhost, sends an order, and receives an execution report. About 150 lines of code.

**Source**: [`examples/quick_start.cpp`](../examples/quick_start.cpp)

```bash
cmake --build build --target nexusfix_quick_start
./build/bin/examples/nexusfix_quick_start
```

Expected output:

```
NexusFIX Quick Start

[SERVER] Listening on port 45025
[CLIENT] Logon complete, session active
[SERVER] Logon complete
[CLIENT] Sent NOS: AAPL BUY 100 @ 150.25
[SERVER] Received NOS, sending ExecutionReport
[CLIENT] ExecutionReport received  order_id=ORD-100  exec_type=0  symbol=AAPL

Done.
```

---

## Step 1: Wire up the server (acceptor)

The acceptor listens for inbound connections. When it receives a NewOrderSingle, it responds with an ExecutionReport.

```cpp
#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"

using namespace nfx;

// Listen on an ephemeral port (port 0 = OS-assigned)
TcpAcceptor listener;
listener.listen(0);
uint16_t port = listener.local_port();
```

Accept a connection in a background thread, create a `SessionManager`, and wire callbacks:

```cpp
auto fd = listener.accept();
TcpSocket client{*fd};

SessionConfig sc;
sc.sender_comp_id = "SERVER";
sc.target_comp_id = "CLIENT";
sc.heart_bt_int = 30;
sc.validate_comp_ids = false;

SessionManager session{sc};

SessionCallbacks cbs;
cbs.on_send = [&client](std::span<const char> data) -> bool {
    auto r = client.send(data);
    return r.has_value() && *r > 0;
};
cbs.on_app_message = [&session](const ParsedMessage& msg) {
    if (msg.msg_type() != msg_type::NewOrderSingle) return;

    auto er = fix44::ExecutionReport::Builder{}
        .order_id("ORD-100")
        .exec_id("EXEC-100")
        .exec_type(ExecType::New)
        .ord_status(OrdStatus::New)
        .symbol(msg.get_string(tag::Symbol::value))
        .side(Side::Buy)
        .leaves_qty(Qty::from_int(100))
        .cum_qty(Qty::from_int(0))
        .avg_px(FixedPrice::from_double(0.0))
        .cl_ord_id(msg.get_string(tag::ClOrdID::value))
        .transact_time("20260618-12:00:00.000");
    (void)session.send_app_message(er);
};
session.set_callbacks(std::move(cbs));
session.on_connect();

// SocketBridge ties the socket to the session: recv -> parse -> dispatch
SocketBridge<> bridge{client, session};
while (client.is_connected()) {
    bridge.poll_once(20);  // 20ms poll timeout
}
```

Key points:
- `on_send` is how the session transmits data. You provide the socket write.
- `on_app_message` fires for application messages (not admin like Logon/Heartbeat).
- `SocketBridge` handles recv, framing, and dispatch in a single `poll_once` call.
- The session handles Logon/Logout/Heartbeat automatically.

---

## Step 2: Connect the client (initiator)

`FixInitiator` wraps `SessionManager` + `TcpSocket` + `SocketBridge` into a single object. Configure it, set callbacks, and call `start()`.

```cpp
#include "nexusfix/engine/fix_initiator.hpp"

InitiatorConfig cfg;
cfg.host = "127.0.0.1";
cfg.port = port;
cfg.sender_comp_id = "CLIENT";
cfg.target_comp_id = "SERVER";
cfg.heart_bt_int = 30;
cfg.validate_comp_ids = false;
cfg.auto_reconnect = false;

FixInitiator initiator{cfg};

SessionCallbacks client_cbs;
client_cbs.on_logon = []() {
    std::cout << "Session active\n";
};
client_cbs.on_app_message = [](const ParsedMessage& msg) {
    if (msg.msg_type() == msg_type::ExecutionReport) {
        std::cout << "ER received: " << msg.get_string(tag::OrderID::value) << "\n";
    }
};
initiator.set_callbacks(std::move(client_cbs));

initiator.start();  // connects + sends Logon
```

---

## Step 3: Wait for logon

`poll_until` drives the event loop until a predicate is true (or timeout):

```cpp
initiator.poll_until([&] { return initiator.is_active(); }, 3000);
```

---

## Step 4: Send a NewOrderSingle

Build the message with the fluent builder API. Strong types (`Qty`, `FixedPrice`, `Side`) prevent field confusion at compile time.

```cpp
#include "nexusfix/messages/fix44/new_order_single.hpp"

auto nos = fix44::NewOrderSingle::Builder{}
    .cl_ord_id("ORD001")
    .symbol("AAPL")
    .side(Side::Buy)
    .transact_time("20260618-12:00:00.000")
    .order_qty(Qty::from_int(100))
    .ord_type(OrdType::Limit)
    .price(FixedPrice::from_double(150.25))
    .time_in_force(TimeInForce::Day);

initiator.send(nos);
```

`send()` serializes the message (header, body, checksum), increments the sequence number, and writes to the socket.

---

## Step 5: Receive the ExecutionReport

Already handled in the `on_app_message` callback from Step 2. Access fields by tag:

```cpp
msg.get_string(tag::OrderID::value)   // "ORD-100"
msg.get_char(tag::ExecType::value)    // '0' (New)
msg.get_char(tag::OrdStatus::value)   // '0' (New)
msg.get_string(tag::Symbol::value)    // "AAPL"
msg.get_int(tag::LeavesQty::value)    // 100
msg.get_price(tag::AvgPx::value)      // FixedPrice
```

---

## Step 6: Logout and shutdown

```cpp
initiator.stop();   // sends Logout
initiator.poll_until(
    [&] { return initiator.state() == SessionState::Disconnected; }, 2000);
```

---

## Architecture overview

```
FixInitiator                          Server thread
  +-----------+                       +-------------+
  | poll()    |---TCP--->| NOS |----->| poll_once() |
  |           |                       | on_app_msg  |
  |           |<--TCP---| ER  |<-----|   send ER   |
  | on_app_msg|                       +-------------+
  +-----------+
```

The initiator is single-threaded: call `poll()` or `poll_until()` in your own event loop. The acceptor side runs in a background thread. Both sides handle heartbeats and sequence numbers automatically.

---

## Next steps

- **Cancel an order**: Use `fix44::OrderCancelRequest::Builder` (see `examples/simple_client.cpp`)
- **Market data**: Use `fix44::MarketDataRequest::Builder` to subscribe
- **Production**: Add a `MemoryMessageStore` for gap-fill/resend support
- **API reference**: See [`docs/API_REFERENCE.md`](API_REFERENCE.md) and [`docs/API_QUICK_START_EN.md`](API_QUICK_START_EN.md)
