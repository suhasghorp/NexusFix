// SPDX-License-Identifier: MIT
// Copyright (c) 2025 StratCraftsAI
//
// TICKET_497 Phase 1: Close error-path branches in messages/.
//
// The FIX message from_buffer() functions are a wall of
//   if (field missing) return std::unexpected{MissingRequiredField, tag};
// early returns. Line coverage counts each as covered once the happy path
// runs; the failure branch stays unexecuted. These tests drive every
// missing-required-field branch and the conditional Price/StopPx branches
// across all four NewOrderSingle versions, plus the InvalidMsgType guard.

#include <catch2/catch_test_macros.hpp>

#include "nexusfix/messages/common/trailer.hpp"
#include "nexusfix/messages/fix42/execution_report.hpp"
#include "nexusfix/messages/fix42/new_order_single.hpp"
#include "nexusfix/messages/fix43/execution_report.hpp"
#include "nexusfix/messages/fix43/new_order_single.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"
#include "nexusfix/messages/fix50/execution_report.hpp"
#include "nexusfix/messages/fix50/new_order_single.hpp"
#include "nexusfix/parser/runtime_parser.hpp"
#include "nexusfix/types/error.hpp"

using namespace nfx;

namespace {

// Bit flags for each required field so a test can omit exactly one.
enum Field : unsigned {
    F_ClOrdID    = 1u << 0,
    F_HandlInst  = 1u << 1,  // FIX 4.2 only
    F_Symbol     = 1u << 2,
    F_Side       = 1u << 3,
    F_TransTime  = 1u << 4,
    F_OrderQty   = 1u << 5,
    F_OrdType    = 1u << 6,
    F_Price      = 1u << 7,
};

// Emit the shared header fields common to every NewOrderSingle version.
void emit_header(MessageAssembler& asm_, char msg_type) {
    asm_.field(tag::MsgType::value, msg_type)
        .field(tag::SenderCompID::value, "S")
        .field(tag::TargetCompID::value, "T")
        .field(tag::MsgSeqNum::value, static_cast<int64_t>(1))
        .field(tag::SendingTime::value, "20260716-10:00:00");
}

// Emit a Limit NewOrderSingle body, skipping the field named in `omit`.
// include_handl_inst adds tag 21 (required for FIX 4.2).
void emit_limit_body(MessageAssembler& asm_, unsigned omit,
                     bool include_handl_inst) {
    if (!(omit & F_ClOrdID)) {
        asm_.field(tag::ClOrdID::value, "CL001");
    }
    if (include_handl_inst && !(omit & F_HandlInst)) {
        asm_.field(tag::HandlInst::value, '1');
    }
    if (!(omit & F_Symbol)) {
        asm_.field(tag::Symbol::value, "AAPL");
    }
    if (!(omit & F_Side)) {
        asm_.field(tag::Side::value, '1');
    }
    if (!(omit & F_TransTime)) {
        asm_.field(tag::TransactTime::value, "20260716-10:00:00");
    }
    if (!(omit & F_OrderQty)) {
        asm_.field(tag::OrderQty::value, static_cast<int64_t>(100));
    }
    if (!(omit & F_OrdType)) {
        asm_.field(tag::OrdType::value, '2');  // Limit
    }
    if (!(omit & F_Price)) {
        asm_.field(tag::Price::value, FixedPrice::from_double(150.0));
    }
}

}  // namespace

// ============================================================================
// FIX 4.2 NewOrderSingle - full missing-field matrix (HandlInst required)
// ============================================================================

TEST_CASE("FIX42 NewOrderSingle missing-field branches", "[messages][fix42][nos][error][regression]") {
    auto expect_missing = [](unsigned omit, int expect_tag) {
        MessageAssembler asm_;
        asm_.start(fix::FIX_4_2);
        emit_header(asm_, fix42::NewOrderSingle::MSG_TYPE);
        emit_limit_body(asm_, omit, /*include_handl_inst=*/true);
        auto raw = asm_.finish();

        auto result = fix42::NewOrderSingle::from_buffer(raw);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
        REQUIRE(result.error().tag == expect_tag);
    };

    SECTION("missing ClOrdID")     { expect_missing(F_ClOrdID,   tag::ClOrdID::value); }
    SECTION("missing HandlInst")   { expect_missing(F_HandlInst, tag::HandlInst::value); }
    SECTION("missing Symbol")      { expect_missing(F_Symbol,    tag::Symbol::value); }
    SECTION("missing Side")        { expect_missing(F_Side,      tag::Side::value); }
    SECTION("missing TransactTime"){ expect_missing(F_TransTime, tag::TransactTime::value); }
    SECTION("missing OrderQty")    { expect_missing(F_OrderQty,  tag::OrderQty::value); }
    SECTION("missing OrdType")     { expect_missing(F_OrdType,   tag::OrdType::value); }
    SECTION("limit missing Price") { expect_missing(F_Price,     tag::Price::value); }
}

// ============================================================================
// FIX 4.3 / 4.4 / 5.0 NewOrderSingle - missing-field matrix (no HandlInst req)
// ============================================================================

template <typename Nos>
static void run_missing_field_matrix(std::string_view begin_string, bool fixt11,
                                     bool include_handl_inst) {
    // The assembler owns the buffer that finish() views; keep it and the parse
    // in the same scope so the span never dangles.
    auto expect_missing = [&](unsigned omit, int expect_tag) {
        MessageAssembler asm_;
        if (fixt11) {
            asm_.start_fixt11();
        } else {
            asm_.start(begin_string);
        }
        emit_header(asm_, Nos::MSG_TYPE);
        emit_limit_body(asm_, omit, include_handl_inst);
        auto raw = asm_.finish();

        auto result = Nos::from_buffer(raw);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
        REQUIRE(result.error().tag == expect_tag);
    };

    expect_missing(F_ClOrdID,   tag::ClOrdID::value);
    if (include_handl_inst) {
        expect_missing(F_HandlInst, tag::HandlInst::value);
    }
    expect_missing(F_Symbol,    tag::Symbol::value);
    expect_missing(F_Side,      tag::Side::value);
    expect_missing(F_TransTime, tag::TransactTime::value);
    expect_missing(F_OrderQty,  tag::OrderQty::value);
    expect_missing(F_OrdType,   tag::OrdType::value);
    expect_missing(F_Price,     tag::Price::value);  // Limit missing Price
}

// FIX 4.3 requires HandlInst (tag 21), same as FIX 4.2. FIX 4.4 and 5.0 do not.
TEST_CASE("FIX43 NewOrderSingle missing-field branches", "[messages][fix43][nos][error][regression]") {
    run_missing_field_matrix<fix43::NewOrderSingle>(fix::FIX_4_3, /*fixt11=*/false,
                                                    /*include_handl_inst=*/true);
}

TEST_CASE("FIX44 NewOrderSingle missing-field branches", "[messages][fix44][nos][error][regression]") {
    run_missing_field_matrix<fix44::NewOrderSingle>(fix::FIX_4_4, /*fixt11=*/false,
                                                    /*include_handl_inst=*/false);
}

TEST_CASE("FIX50 NewOrderSingle missing-field branches", "[messages][fix50][nos][error][regression]") {
    run_missing_field_matrix<fix50::NewOrderSingle>(fix_version::FIXT_1_1, /*fixt11=*/true,
                                                    /*include_handl_inst=*/false);
}

// ============================================================================
// Stop-order conditional branch (is_stop && stop_px == 0) across versions
// ============================================================================

template <typename Nos>
static void run_stop_missing_stoppx(std::string_view begin_string, bool fixt11,
                                    bool include_handl_inst) {
    MessageAssembler asm_;
    if (fixt11) {
        asm_.start_fixt11();
    } else {
        asm_.start(begin_string);
    }
    emit_header(asm_, Nos::MSG_TYPE);
    asm_.field(tag::ClOrdID::value, "CL001");
    if (include_handl_inst) {
        asm_.field(tag::HandlInst::value, '1');
    }
    asm_.field(tag::Symbol::value, "AAPL")
        .field(tag::Side::value, '1')
        .field(tag::TransactTime::value, "20260716-10:00:00")
        .field(tag::OrderQty::value, static_cast<int64_t>(100))
        .field(tag::OrdType::value, '3');  // Stop, no StopPx
    auto raw = asm_.finish();

    auto result = Nos::from_buffer(raw);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
    REQUIRE(result.error().tag == tag::StopPx::value);
}

TEST_CASE("NewOrderSingle stop order missing StopPx", "[messages][nos][error][regression]") {
    SECTION("FIX42") {
        run_stop_missing_stoppx<fix42::NewOrderSingle>(fix::FIX_4_2, false, true);
    }
    SECTION("FIX43") {
        run_stop_missing_stoppx<fix43::NewOrderSingle>(fix::FIX_4_3, false, true);
    }
    SECTION("FIX44") {
        run_stop_missing_stoppx<fix44::NewOrderSingle>(fix::FIX_4_4, false, false);
    }
    SECTION("FIX50") {
        run_stop_missing_stoppx<fix50::NewOrderSingle>(fix_version::FIXT_1_1, true, false);
    }
}

// StopLimit order exercises both is_limit() and is_stop() true sub-branches.
TEST_CASE("NewOrderSingle StopLimit exercises is_limit and is_stop", "[messages][nos][regression]") {
    MessageAssembler asm_;
    asm_.start(fix::FIX_4_4);
    emit_header(asm_, fix44::NewOrderSingle::MSG_TYPE);
    asm_.field(tag::ClOrdID::value, "CL001")
        .field(tag::Symbol::value, "AAPL")
        .field(tag::Side::value, '1')
        .field(tag::TransactTime::value, "20260716-10:00:00")
        .field(tag::OrderQty::value, static_cast<int64_t>(100))
        .field(tag::OrdType::value, '4')  // StopLimit
        .field(tag::Price::value, FixedPrice::from_double(150.0))
        .field(tag::StopPx::value, FixedPrice::from_double(149.0));
    auto raw = asm_.finish();

    auto result = fix44::NewOrderSingle::from_buffer(raw);
    REQUIRE(result.has_value());
    REQUIRE(result->is_limit());
    REQUIRE(result->is_stop());
    REQUIRE_FALSE(result->is_market());
}

// ============================================================================
// InvalidMsgType guard: a well-formed message with the wrong 35= value
// ============================================================================

TEST_CASE("NewOrderSingle from_buffer rejects wrong MsgType", "[messages][nos][error][regression]") {
    // Build a syntactically valid Heartbeat (35=0), parse as NewOrderSingle.
    MessageAssembler asm_;
    asm_.start(fix::FIX_4_4);
    emit_header(asm_, '0');  // Heartbeat, not 'D'
    auto raw = asm_.finish();

    SECTION("FIX42") {
        auto r = fix42::NewOrderSingle::from_buffer(raw);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ParseErrorCode::InvalidMsgType);
    }
    SECTION("FIX43") {
        auto r = fix43::NewOrderSingle::from_buffer(raw);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ParseErrorCode::InvalidMsgType);
    }
    SECTION("FIX44") {
        auto r = fix44::NewOrderSingle::from_buffer(raw);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ParseErrorCode::InvalidMsgType);
    }
    SECTION("FIX50") {
        auto r = fix50::NewOrderSingle::from_buffer(raw);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ParseErrorCode::InvalidMsgType);
    }
}

// ============================================================================
// Builder optional-field branches: present vs absent for each optional tag.
// The Builder emits optional fields only when non-empty/non-zero; a round-trip
// with all optionals set drives the "present" branch, the minimal build drives
// the "absent" branch.
// ============================================================================

TEST_CASE("FIX44 NewOrderSingle Builder optional-field branches", "[messages][fix44][nos][builder][regression]") {
    SECTION("all optionals present") {
        MessageAssembler asm_;
        auto raw = fix44::NewOrderSingle::Builder{}
            .sender_comp_id("C").target_comp_id("B").msg_seq_num(1)
            .sending_time("20260716-10:00:00")
            .cl_ord_id("CL001").symbol("AAPL").side(Side::Buy)
            .transact_time("20260716-10:00:00")
            .order_qty(Qty::from_int(100)).ord_type(OrdType::StopLimit)
            .price(FixedPrice::from_double(150.0))
            .stop_px(FixedPrice::from_double(149.0))
            .account("ACCT1").handl_inst('1')
            .ex_destination("NASDAQ").text("hello")
            .build(asm_);

        auto result = fix44::NewOrderSingle::from_buffer(raw);
        REQUIRE(result.has_value());
        REQUIRE(result->account == "ACCT1");
        REQUIRE(result->ex_destination == "NASDAQ");
        REQUIRE(result->text == "hello");
        REQUIRE(result->stop_px.raw != 0);
    }

    SECTION("all optionals absent") {
        MessageAssembler asm_;
        auto raw = fix44::NewOrderSingle::Builder{}
            .sender_comp_id("C").target_comp_id("B").msg_seq_num(1)
            .sending_time("20260716-10:00:00")
            .cl_ord_id("CL002").symbol("MSFT").side(Side::Sell)
            .transact_time("20260716-10:00:00")
            .order_qty(Qty::from_int(50)).ord_type(OrdType::Market)
            .handl_inst('\0')  // suppress HandlInst emission
            .build(asm_);

        auto result = fix44::NewOrderSingle::from_buffer(raw);
        REQUIRE(result.has_value());
        REQUIRE(result->is_market());
        REQUIRE(result->account.empty());
        REQUIRE(result->ex_destination.empty());
        REQUIRE(result->text.empty());
    }
}

// ============================================================================
// ExecutionReport missing-field branches (TICKET_497 Phase 1)
// ============================================================================
// Required fields: OrderID, ExecID, ExecType, OrdStatus, Symbol, Side.
// FIX 4.2 and 4.3 additionally require ExecTransType (tag 20).

namespace {

enum ErField : unsigned {
    E_OrderID       = 1u << 0,
    E_ExecID        = 1u << 1,
    E_ExecTransType = 1u << 2,  // FIX 4.2 / 4.3 only
    E_ExecType      = 1u << 3,
    E_OrdStatus     = 1u << 4,
    E_Symbol        = 1u << 5,
    E_Side          = 1u << 6,
};

constexpr int TAG_ExecTransType = 20;

void emit_exec_report_body(MessageAssembler& asm_, unsigned omit,
                           bool include_exec_trans_type) {
    if (!(omit & E_OrderID)) {
        asm_.field(tag::OrderID::value, "ORD1");
    }
    if (!(omit & E_ExecID)) {
        asm_.field(tag::ExecID::value, "EX1");
    }
    if (include_exec_trans_type && !(omit & E_ExecTransType)) {
        asm_.field(TAG_ExecTransType, '0');  // ExecTransType::New
    }
    if (!(omit & E_ExecType)) {
        asm_.field(tag::ExecType::value, '2');  // Fill
    }
    if (!(omit & E_OrdStatus)) {
        asm_.field(tag::OrdStatus::value, '2');  // Filled
    }
    if (!(omit & E_Symbol)) {
        asm_.field(tag::Symbol::value, "AAPL");
    }
    if (!(omit & E_Side)) {
        asm_.field(tag::Side::value, '1');
    }
    // Quantity/price fields below are optional or defaulted in every version.
    asm_.field(tag::LeavesQty::value, static_cast<int64_t>(0))
        .field(tag::CumQty::value, static_cast<int64_t>(100))
        .field(tag::AvgPx::value, FixedPrice::from_double(150.0));
}

}  // namespace

template <typename Er>
static void run_exec_report_matrix(std::string_view begin_string, bool fixt11,
                                   bool include_exec_trans_type) {
    auto expect_missing = [&](unsigned omit, int expect_tag) {
        MessageAssembler asm_;
        if (fixt11) {
            asm_.start_fixt11();
        } else {
            asm_.start(begin_string);
        }
        emit_header(asm_, Er::MSG_TYPE);
        emit_exec_report_body(asm_, omit, include_exec_trans_type);
        auto raw = asm_.finish();

        auto result = Er::from_buffer(raw);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == ParseErrorCode::MissingRequiredField);
        REQUIRE(result.error().tag == expect_tag);
    };

    expect_missing(E_OrderID,   tag::OrderID::value);
    expect_missing(E_ExecID,    tag::ExecID::value);
    if (include_exec_trans_type) {
        expect_missing(E_ExecTransType, TAG_ExecTransType);
    }
    expect_missing(E_ExecType,  tag::ExecType::value);
    expect_missing(E_OrdStatus, tag::OrdStatus::value);
    expect_missing(E_Symbol,    tag::Symbol::value);
    expect_missing(E_Side,      tag::Side::value);
}

TEST_CASE("FIX42 ExecutionReport missing-field branches", "[messages][fix42][exec_report][error][regression]") {
    run_exec_report_matrix<fix42::ExecutionReport>(fix::FIX_4_2, false, true);
}

TEST_CASE("FIX43 ExecutionReport missing-field branches", "[messages][fix43][exec_report][error][regression]") {
    run_exec_report_matrix<fix43::ExecutionReport>(fix::FIX_4_3, false, true);
}

TEST_CASE("FIX44 ExecutionReport missing-field branches", "[messages][fix44][exec_report][error][regression]") {
    run_exec_report_matrix<fix44::ExecutionReport>(fix::FIX_4_4, false, false);
}

TEST_CASE("FIX50 ExecutionReport missing-field branches", "[messages][fix50][exec_report][error][regression]") {
    run_exec_report_matrix<fix50::ExecutionReport>(fix_version::FIXT_1_1, true, false);
}

TEST_CASE("ExecutionReport from_buffer rejects wrong MsgType", "[messages][exec_report][error][regression]") {
    MessageAssembler asm_;
    asm_.start(fix::FIX_4_4);
    emit_header(asm_, '0');  // Heartbeat, not '8'
    auto raw = asm_.finish();

    auto r = fix44::ExecutionReport::from_buffer(raw);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ParseErrorCode::InvalidMsgType);
}

// is_fill()/is_rejected() carry two-operand short-circuits: PartialFill and the
// OrdStatus::Rejected side stay unexecuted when only Fill/Filled is ever built.
TEST_CASE("ExecutionReport convenience predicate sub-branches", "[messages][exec_report][regression]") {
    // Keep the assembler in scope alongside the parse; finish() views its
    // internal buffer and must outlive from_buffer().
    auto emit = [](MessageAssembler& asm_, char exec_type, char ord_status) {
        asm_.start(fix::FIX_4_4);
        emit_header(asm_, fix44::ExecutionReport::MSG_TYPE);
        asm_.field(tag::OrderID::value, "ORD1")
            .field(tag::ExecID::value, "EX1")
            .field(tag::ExecType::value, exec_type)
            .field(tag::OrdStatus::value, ord_status)
            .field(tag::Symbol::value, "AAPL")
            .field(tag::Side::value, '1')
            .field(tag::LeavesQty::value, static_cast<int64_t>(50))
            .field(tag::CumQty::value, static_cast<int64_t>(50))
            .field(tag::AvgPx::value, FixedPrice::from_double(150.0));
        return asm_.finish();
    };

    SECTION("PartialFill drives is_fill second operand") {
        MessageAssembler asm_;
        auto raw = emit(asm_, '1', '1');  // ExecType::PartialFill, OrdStatus::PartiallyFilled
        auto r = fix44::ExecutionReport::from_buffer(raw);
        REQUIRE(r.has_value());
        REQUIRE(r->is_fill());
        REQUIRE_FALSE(r->is_rejected());
    }

    SECTION("Rejected status drives is_rejected second operand") {
        // ExecType New ('0') but OrdStatus Rejected ('8') exercises the
        // ord_status == Rejected side of the || without the exec_type side.
        MessageAssembler asm_;
        auto raw = emit(asm_, '0', '8');
        auto r = fix44::ExecutionReport::from_buffer(raw);
        REQUIRE(r.has_value());
        REQUIRE(r->is_rejected());
        REQUIRE_FALSE(r->is_fill());
    }
}
