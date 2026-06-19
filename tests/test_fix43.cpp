#include <catch2/catch_test_macros.hpp>
#include <string>

#include "nexusfix/messages/fix43/fix43.hpp"
#include "nexusfix/messages/fix44/logon.hpp"
#include "nexusfix/messages/fix44/heartbeat.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"

using namespace nfx;
using namespace nfx::fix43;

// ============================================================================
// FIX 4.3 ExecutionReport Tests
// ============================================================================

TEST_CASE("FIX 4.3 ExecutionReport round-trip", "[fix43][exec_report][regression]") {
    MessageAssembler asm_;
    auto raw = ExecutionReport::Builder{}
        .sender_comp_id("BROKER")
        .target_comp_id("CLIENT")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .order_id("ORD001")
        .exec_id("EXEC001")
        .exec_trans_type(ExecTransType::New)
        .exec_type(ExecType::New)
        .ord_status(OrdStatus::New)
        .symbol("600000.SH")
        .side(Side::Buy)
        .leaves_qty(Qty::from_int(1000))
        .cum_qty(Qty::from_int(0))
        .avg_px(FixedPrice::from_double(0.0))
        .order_qty(Qty::from_int(1000))
        .cl_ord_id("CL001")
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = ExecutionReport::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.header.begin_string == "FIX.4.3");
    REQUIRE(msg.exec_trans_type == ExecTransType::New);
    REQUIRE(msg.exec_type == ExecType::New);
    REQUIRE(msg.ord_status == OrdStatus::New);
    REQUIRE(msg.symbol == "600000.SH");
    REQUIRE(msg.side == Side::Buy);
    REQUIRE(msg.order_id == "ORD001");
    REQUIRE(msg.exec_id == "EXEC001");
    REQUIRE(msg.cl_ord_id == "CL001");
    REQUIRE(msg.leaves_qty.whole() == 1000);
    REQUIRE(msg.order_qty.whole() == 1000);
}

TEST_CASE("FIX 4.3 ExecutionReport with PartialFill and Fill", "[fix43][exec_report][regression]") {
    SECTION("PartialFill") {
        MessageAssembler asm_;
        auto raw = ExecutionReport::Builder{}
            .sender_comp_id("BROKER")
            .target_comp_id("CLIENT")
            .msg_seq_num(2)
            .sending_time("20260618-10:00:01.000")
            .order_id("ORD001")
            .exec_id("EXEC002")
            .exec_trans_type(ExecTransType::New)
            .exec_type(ExecType::PartialFill)
            .ord_status(OrdStatus::PartiallyFilled)
            .symbol("600000.SH")
            .side(Side::Buy)
            .leaves_qty(Qty::from_int(500))
            .cum_qty(Qty::from_int(500))
            .avg_px(FixedPrice::from_double(10.50))
            .last_qty(Qty::from_int(500))
            .last_px(FixedPrice::from_double(10.50))
            .build(asm_);

        auto result = ExecutionReport::from_buffer(raw);
        REQUIRE(result.has_value());

        auto& msg = *result;
        REQUIRE(msg.exec_type == ExecType::PartialFill);
        REQUIRE(msg.ord_status == OrdStatus::PartiallyFilled);
        REQUIRE(msg.is_fill());
        REQUIRE(!msg.is_terminal());
        REQUIRE(msg.leaves_qty.whole() == 500);
        REQUIRE(msg.cum_qty.whole() == 500);
        REQUIRE(msg.last_qty.whole() == 500);
    }

    SECTION("Fill") {
        MessageAssembler asm_;
        auto raw = ExecutionReport::Builder{}
            .sender_comp_id("BROKER")
            .target_comp_id("CLIENT")
            .msg_seq_num(3)
            .sending_time("20260618-10:00:02.000")
            .order_id("ORD001")
            .exec_id("EXEC003")
            .exec_trans_type(ExecTransType::New)
            .exec_type(ExecType::Fill)
            .ord_status(OrdStatus::Filled)
            .symbol("600000.SH")
            .side(Side::Buy)
            .leaves_qty(Qty::from_int(0))
            .cum_qty(Qty::from_int(1000))
            .avg_px(FixedPrice::from_double(10.50))
            .last_qty(Qty::from_int(500))
            .last_px(FixedPrice::from_double(10.50))
            .build(asm_);

        auto result = ExecutionReport::from_buffer(raw);
        REQUIRE(result.has_value());

        auto& msg = *result;
        REQUIRE(msg.exec_type == ExecType::Fill);
        REQUIRE(msg.ord_status == OrdStatus::Filled);
        REQUIRE(msg.is_fill());
        REQUIRE(msg.is_terminal());
    }
}

TEST_CASE("FIX 4.3 ExecutionReport with rejection", "[fix43][exec_report][regression]") {
    MessageAssembler asm_;
    auto raw = ExecutionReport::Builder{}
        .sender_comp_id("BROKER")
        .target_comp_id("CLIENT")
        .msg_seq_num(4)
        .sending_time("20260618-10:00:03.000")
        .order_id("ORD002")
        .exec_id("EXEC004")
        .exec_trans_type(ExecTransType::New)
        .exec_type(ExecType::Rejected)
        .ord_status(OrdStatus::Rejected)
        .symbol("TSLA")
        .side(Side::Sell)
        .leaves_qty(Qty::from_int(0))
        .cum_qty(Qty::from_int(0))
        .avg_px(FixedPrice::from_double(0.0))
        .ord_rej_reason(3)
        .text("Insufficient buying power")
        .build(asm_);

    auto result = ExecutionReport::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.is_rejected());
    REQUIRE(msg.ord_rej_reason == 3);
    REQUIRE(msg.text == "Insufficient buying power");
}

// ============================================================================
// FIX 4.3 NewOrderSingle Tests
// ============================================================================

TEST_CASE("FIX 4.3 NewOrderSingle round-trip", "[fix43][nos][regression]") {
    MessageAssembler asm_;
    auto raw = NewOrderSingle::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .cl_ord_id("ORD001")
        .handl_inst('1')
        .symbol("600000.SH")
        .side(Side::Buy)
        .transact_time("20260618-10:00:00.000")
        .order_qty(Qty::from_int(1000))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(10.50))
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = NewOrderSingle::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.header.begin_string == "FIX.4.3");
    REQUIRE(msg.header.msg_type == 'D');
    REQUIRE(msg.cl_ord_id == "ORD001");
    REQUIRE(msg.handl_inst == '1');
    REQUIRE(msg.symbol == "600000.SH");
    REQUIRE(msg.side == Side::Buy);
    REQUIRE(msg.order_qty.whole() == 1000);
    REQUIRE(msg.ord_type == OrdType::Limit);
}

TEST_CASE("FIX 4.3 NewOrderSingle with Product tag", "[fix43][nos][regression]") {
    MessageAssembler asm_;
    auto raw = NewOrderSingle::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .cl_ord_id("ORD002")
        .handl_inst('1')
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260618-10:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.25))
        .product(4)
        .build(asm_);

    auto result = NewOrderSingle::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.product == 4);

    std::string_view wire(raw.data(), raw.size());
    REQUIRE(wire.find("460=4") != std::string_view::npos);
}

TEST_CASE("FIX 4.3 NewOrderSingle rejects missing HandlInst", "[fix43][nos][regression]") {
    MessageAssembler asm_;
    auto raw = nfx::fix44::NewOrderSingle::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .cl_ord_id("ORD001")
        .symbol("600000.SH")
        .side(Side::Buy)
        .transact_time("20260618-10:00:00.000")
        .order_qty(Qty::from_int(1000))
        .ord_type(OrdType::Market)
        .handl_inst('\0')
        .build(asm_);

    auto result = fix43::NewOrderSingle::from_buffer(raw);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
    REQUIRE(result.error().tag == tag::HandlInst::value);
}

// ============================================================================
// FIX 4.3 OrderCancelRequest Tests
// ============================================================================

TEST_CASE("FIX 4.3 OrderCancelRequest round-trip", "[fix43][cancel][regression]") {
    MessageAssembler asm_;
    auto raw = OrderCancelRequest::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(5)
        .sending_time("20260618-10:00:05.000")
        .orig_cl_ord_id("ORD001")
        .cl_ord_id("CXL001")
        .symbol("600000.SH")
        .side(Side::Buy)
        .transact_time("20260618-10:00:05.000")
        .order_qty(Qty::from_int(1000))
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = OrderCancelRequest::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.header.begin_string == "FIX.4.3");
    REQUIRE(msg.orig_cl_ord_id == "ORD001");
    REQUIRE(msg.cl_ord_id == "CXL001");
    REQUIRE(msg.symbol == "600000.SH");
    REQUIRE(msg.side == Side::Buy);
    REQUIRE(msg.order_qty.whole() == 1000);
}

// ============================================================================
// FIX 4.3 MarketDataRequest Tests
// ============================================================================

TEST_CASE("FIX 4.3 MarketDataRequest round-trip", "[fix43][market_data][regression]") {
    MessageAssembler asm_;
    auto raw = MarketDataRequest::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .md_req_id("MDR001")
        .subscription_type(SubscriptionRequestType::SnapshotPlusUpdates)
        .market_depth(5)
        .md_update_type(MDUpdateType::IncrementalRefresh)
        .add_entry_type(MDEntryType::Bid)
        .add_entry_type(MDEntryType::Offer)
        .add_symbol("AAPL")
        .add_symbol("MSFT")
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = MarketDataRequest::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.header.begin_string == "FIX.4.3");
    REQUIRE(msg.md_req_id == "MDR001");
    REQUIRE(msg.is_subscribe());
    REQUIRE(!msg.is_snapshot());
    REQUIRE(msg.market_depth == 5);
    REQUIRE(msg.no_md_entry_types == 2);
    REQUIRE(msg.no_related_sym == 2);
}

TEST_CASE("FIX 4.3 MarketDataRequest snapshot", "[fix43][market_data][regression]") {
    MessageAssembler asm_;
    auto raw = MarketDataRequest::Builder{}
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(2)
        .sending_time("20260618-10:00:01.000")
        .md_req_id("MDR002")
        .subscription_type(SubscriptionRequestType::Snapshot)
        .market_depth(0)
        .add_entry_type(MDEntryType::Bid)
        .add_entry_type(MDEntryType::Offer)
        .add_entry_type(MDEntryType::Trade)
        .add_symbol("TSLA")
        .build(asm_);

    auto result = MarketDataRequest::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.is_snapshot());
    REQUIRE(!msg.is_subscribe());
    REQUIRE(!msg.is_unsubscribe());
    REQUIRE(msg.no_md_entry_types == 3);
    REQUIRE(msg.no_related_sym == 1);
}

// ============================================================================
// FIX 4.3 MarketDataSnapshotFullRefresh Tests
// ============================================================================

TEST_CASE("FIX 4.3 MarketDataSnapshotFullRefresh round-trip", "[fix43][market_data][regression]") {
    MessageAssembler asm_;
    auto raw = MarketDataSnapshotFullRefresh::Builder{}
        .sender_comp_id("EXCHANGE")
        .target_comp_id("CLIENT")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .md_req_id("MDR001")
        .symbol("AAPL")
        .add_entry(MDEntryType::Bid, FixedPrice::from_double(150.00), Qty::from_int(500))
        .add_entry(MDEntryType::Offer, FixedPrice::from_double(150.10), Qty::from_int(300))
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = MarketDataSnapshotFullRefresh::from_buffer(raw);
    REQUIRE(result.has_value());

    auto& msg = *result;
    REQUIRE(msg.header.begin_string == "FIX.4.3");
    REQUIRE(msg.md_req_id == "MDR001");
    REQUIRE(msg.symbol == "AAPL");
    REQUIRE(msg.entry_count() == 2);
}

TEST_CASE("FIX 4.3 MarketDataSnapshotFullRefresh rejects missing symbol", "[fix43][market_data][regression]") {
    MessageAssembler asm_;
    asm_.start(fix::FIX_4_3)
        .field(tag::MsgType::value, 'W')
        .field(tag::SenderCompID::value, "EXCHANGE")
        .field(tag::TargetCompID::value, "CLIENT")
        .field(tag::MsgSeqNum::value, static_cast<int64_t>(1))
        .field(tag::SendingTime::value, "20260618-10:00:00.000")
        .field(tag::MDReqID::value, "MDR001");
    auto raw = asm_.finish();

    auto result = MarketDataSnapshotFullRefresh::from_buffer(raw);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
    REQUIRE(result.error().tag == tag::Symbol::value);
}

// ============================================================================
// FIX 4.3 Session with Logon (begin_string=FIX.4.3)
// ============================================================================

TEST_CASE("Session builder with begin_string=FIX.4.3 produces correct Logon", "[fix43][session][regression]") {
    MessageAssembler asm_;
    auto raw = nfx::fix44::Logon::Builder{}
        .begin_string(fix::FIX_4_3)
        .sender_comp_id("CLIENT")
        .target_comp_id("EXCHANGE")
        .msg_seq_num(1)
        .sending_time("20260618-10:00:00.000")
        .encrypt_method(0)
        .heart_bt_int(30)
        .build(asm_);

    std::string_view raw_sv{raw.data(), raw.size()};
    REQUIRE(raw_sv.starts_with("8=FIX.4.3"));

    auto result = nfx::fix44::Logon::from_buffer(raw);
    REQUIRE(result.has_value());
    REQUIRE(result->header.begin_string == "FIX.4.3");
    REQUIRE(result->encrypt_method == 0);
    REQUIRE(result->heart_bt_int == 30);
}
