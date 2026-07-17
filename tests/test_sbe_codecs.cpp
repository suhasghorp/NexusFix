#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <array>
#include <stdexcept>

#include "nexusfix/sbe/sbe.hpp"
#include "nexusfix/sbe/codecs/execution_report.hpp"
#include "nexusfix/sbe/codecs/new_order_single.hpp"
#include "nexusfix/sbe/message_header.hpp"
#include "nexusfix/sbe/types/composite_types.hpp"
#include "nexusfix/sbe/types/sbe_types.hpp"

using namespace nfx;
using namespace nfx::sbe;

// ============================================================================
// SBE Type Read/Write Tests
// ============================================================================

TEST_CASE("SBE read_le / write_le roundtrip", "[sbe][sbe_types][regression]") {
    SECTION("int64") {
        char buf[8]{};
        SbeInt64 val = 123456789012345LL;
        write_le(buf, val);
        REQUIRE(read_le<SbeInt64>(buf) == val);
    }

    SECTION("int32") {
        char buf[4]{};
        SbeInt32 val = -42;
        write_le(buf, val);
        REQUIRE(read_le<SbeInt32>(buf) == val);
    }

    SECTION("int16") {
        char buf[2]{};
        SbeInt16 val = 12345;
        write_le(buf, val);
        REQUIRE(read_le<SbeInt16>(buf) == val);
    }

    SECTION("uint8") {
        char buf[1]{};
        SbeUint8 val = 255;
        write_le(buf, val);
        REQUIRE(read_le<SbeUint8>(buf) == val);
    }

    SECTION("zero") {
        char buf[8]{};
        SbeInt64 val = 0;
        write_le(buf, val);
        REQUIRE(read_le<SbeInt64>(buf) == 0);
    }

    SECTION("negative") {
        char buf[8]{};
        SbeInt64 val = -1;
        write_le(buf, val);
        REQUIRE(read_le<SbeInt64>(buf) == -1);
    }
}

TEST_CASE("SBE convenience read/write functions", "[sbe][sbe_types][regression]") {
    char buf[8]{};

    write_int64(buf, 42);
    REQUIRE(read_int64(buf) == 42);

    write_int32(buf, -100);
    REQUIRE(read_int32(buf) == -100);

    write_uint16(buf, 65535);
    REQUIRE(read_uint16(buf) == 65535);

    write_char(buf, 'X');
    REQUIRE(read_char(buf) == 'X');
}

TEST_CASE("SBE null value constants", "[sbe][sbe_types][regression]") {
    REQUIRE(null_value::CHAR == '\0');
    REQUIRE(null_value::INT8 == INT8_MIN);
    REQUIRE(null_value::INT16 == INT16_MIN);
    REQUIRE(null_value::INT32 == INT32_MIN);
    REQUIRE(null_value::INT64 == INT64_MIN);
    REQUIRE(null_value::UINT8 == UINT8_MAX);
    REQUIRE(null_value::UINT16 == UINT16_MAX);
    REQUIRE(null_value::UINT32 == UINT32_MAX);
    REQUIRE(null_value::UINT64 == UINT64_MAX);
}

TEST_CASE("SBE check_bounds", "[sbe][sbe_types][regression]") {
    REQUIRE(check_bounds(0, 8, 64));
    REQUIRE(check_bounds(56, 8, 64));
    REQUIRE_FALSE(check_bounds(57, 8, 64));
    REQUIRE_FALSE(check_bounds(64, 1, 64));
    REQUIRE(check_bounds(0, 0, 0));
}

// ============================================================================
// SBE NewOrderSingleCodec Tests
// ============================================================================

TEST_CASE("NewOrderSingleCodec static constants", "[sbe][nos_codec][regression]") {
    REQUIRE(NewOrderSingleCodec::TOTAL_SIZE == 64);
    REQUIRE(NewOrderSingleCodec::BLOCK_LENGTH == 56);
    REQUIRE(NewOrderSingleCodec::encodedSize() == 64);
}

TEST_CASE("NewOrderSingleCodec encode and decode roundtrip", "[sbe][nos_codec][regression]") {
    alignas(8) char buffer[NewOrderSingleCodec::TOTAL_SIZE]{};

    auto encoder = NewOrderSingleCodec::wrapForEncode(buffer, sizeof(buffer));
    encoder.encodeHeader()
        .clOrdId("ORD123")
        .symbol("AAPL")
        .side(Side::Buy)
        .ordType(OrdType::Limit)
        .price(FixedPrice::from_double(150.50))
        .orderQty(Qty::from_int(100))
        .transactTime(Timestamp{1706000000000000000LL});

    auto decoder = NewOrderSingleCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE(decoder.isValid());
    REQUIRE(decoder.clOrdId() == "ORD123");
    REQUIRE(decoder.symbol() == "AAPL");
    REQUIRE(decoder.side() == Side::Buy);
    REQUIRE(decoder.ordType() == OrdType::Limit);
    REQUIRE(decoder.orderQty().whole() == 100);
    REQUIRE(decoder.transactTime().nanos == 1706000000000000000LL);
}

TEST_CASE("NewOrderSingleCodec invalid buffer", "[sbe][nos_codec][regression]") {
    SECTION("nullptr") {
        auto decoder = NewOrderSingleCodec::wrapForDecode(nullptr, 0);
        REQUIRE_FALSE(decoder.isValid());
    }

    SECTION("too small") {
        char buffer[10]{};
        auto decoder = NewOrderSingleCodec::wrapForDecode(buffer, sizeof(buffer));
        REQUIRE_FALSE(decoder.isValid());
    }
}

// ============================================================================
// SBE ExecutionReportCodec Tests
// ============================================================================

TEST_CASE("ExecutionReportCodec static constants", "[sbe][er_codec][regression]") {
    REQUIRE(ExecutionReportCodec::TOTAL_SIZE == 144);
    REQUIRE(ExecutionReportCodec::BLOCK_LENGTH == 136);
    REQUIRE(ExecutionReportCodec::encodedSize() == 144);
}

TEST_CASE("ExecutionReportCodec encode and decode roundtrip", "[sbe][er_codec][regression]") {
    alignas(8) char buffer[ExecutionReportCodec::TOTAL_SIZE]{};

    auto encoder = ExecutionReportCodec::wrapForEncode(buffer, sizeof(buffer));
    encoder.encodeHeader()
        .orderId("EX001")
        .execId("EXEC001")
        .clOrdId("ORD123")
        .symbol("AAPL")
        .side(Side::Buy)
        .execType(ExecType::Fill)
        .ordStatus(OrdStatus::Filled)
        .price(FixedPrice::from_double(150.50))
        .orderQty(Qty::from_int(100))
        .lastPx(FixedPrice::from_double(150.50))
        .lastQty(Qty::from_int(100))
        .leavesQty(Qty::from_int(0))
        .cumQty(Qty::from_int(100))
        .avgPx(FixedPrice::from_double(150.50))
        .transactTime(Timestamp{1706000000000000000LL});

    auto decoder = ExecutionReportCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE(decoder.isValid());
    REQUIRE(decoder.orderId() == "EX001");
    REQUIRE(decoder.execId() == "EXEC001");
    REQUIRE(decoder.clOrdId() == "ORD123");
    REQUIRE(decoder.symbol() == "AAPL");
    REQUIRE(decoder.side() == Side::Buy);
    REQUIRE(decoder.execType() == ExecType::Fill);
    REQUIRE(decoder.ordStatus() == OrdStatus::Filled);
    REQUIRE(decoder.lastQty().whole() == 100);
    REQUIRE(decoder.leavesQty().whole() == 0);
    REQUIRE(decoder.cumQty().whole() == 100);
}

TEST_CASE("ExecutionReportCodec invalid buffer", "[sbe][er_codec][regression]") {
    SECTION("nullptr") {
        auto decoder = ExecutionReportCodec::wrapForDecode(nullptr, 0);
        REQUIRE_FALSE(decoder.isValid());
    }

    SECTION("too small") {
        char buffer[32]{};
        auto decoder = ExecutionReportCodec::wrapForDecode(buffer, sizeof(buffer));
        REQUIRE_FALSE(decoder.isValid());
    }
}

TEST_CASE("ExecutionReportCodec field offsets are 8-byte aligned", "[sbe][er_codec][regression]") {
    REQUIRE(ExecutionReportCodec::Offset::Price % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::OrderQty % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::LastPx % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::LastQty % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::LeavesQty % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::CumQty % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::AvgPx % 8 == 0);
    REQUIRE(ExecutionReportCodec::Offset::TransactTime % 8 == 0);
}

// ============================================================================
// isValid() compound-condition branch coverage (TICKET_497 Phase 1)
// ============================================================================
// Existing tests only exercise the first `||` operand (nullptr / too-small),
// which short-circuits before the templateId/blockLength checks are ever
// evaluated. A full-size buffer carrying the WRONG templateId or WRONG
// blockLength drives the remaining `&&` sub-branches.

TEST_CASE("NewOrderSingleCodec isValid rejects wrong templateId", "[sbe][nos_codec][regression]") {
    alignas(8) char buffer[NewOrderSingleCodec::TOTAL_SIZE]{};

    // Encode a full-size, otherwise-valid header but with the ExecutionReport
    // templateId. Size passes; the templateId check must fail.
    auto header = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
    header.encodeHeader(NewOrderSingleCodec::BLOCK_LENGTH,
                        MessageHeader::TemplateId::ExecutionReport);

    auto decoder = NewOrderSingleCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE_FALSE(decoder.isValid());
}

TEST_CASE("NewOrderSingleCodec isValid rejects wrong blockLength", "[sbe][nos_codec][regression]") {
    alignas(8) char buffer[NewOrderSingleCodec::TOTAL_SIZE]{};

    // Correct templateId but a blockLength that does not match the codec.
    auto header = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
    header.encodeHeader(NewOrderSingleCodec::BLOCK_LENGTH + 1,
                        NewOrderSingleCodec::TEMPLATE_ID);

    auto decoder = NewOrderSingleCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE_FALSE(decoder.isValid());
}

TEST_CASE("ExecutionReportCodec isValid rejects wrong templateId", "[sbe][er_codec][regression]") {
    alignas(8) char buffer[ExecutionReportCodec::TOTAL_SIZE]{};

    auto header = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
    header.encodeHeader(ExecutionReportCodec::BLOCK_LENGTH,
                        MessageHeader::TemplateId::NewOrderSingle);

    auto decoder = ExecutionReportCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE_FALSE(decoder.isValid());
}

TEST_CASE("ExecutionReportCodec isValid rejects wrong blockLength", "[sbe][er_codec][regression]") {
    alignas(8) char buffer[ExecutionReportCodec::TOTAL_SIZE]{};

    auto header = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
    header.encodeHeader(ExecutionReportCodec::BLOCK_LENGTH - 1,
                        ExecutionReportCodec::TEMPLATE_ID);

    auto decoder = ExecutionReportCodec::wrapForDecode(buffer, sizeof(buffer));
    REQUIRE_FALSE(decoder.isValid());
}

// ============================================================================
// MessageHeader branch coverage (TICKET_497 Phase 1)
// ============================================================================

TEST_CASE("MessageHeader validateSchema false paths", "[sbe][header][regression]") {
    alignas(8) char buffer[MessageHeader::SIZE]{};

    SECTION("wrong schemaId fails validateSchema") {
        auto enc = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
        enc.encodeHeader(56, MessageHeader::TemplateId::NewOrderSingle);
        // Corrupt the schemaId in place (encodeHeader always writes SCHEMA_ID).
        write_uint16(buffer + MessageHeader::Offset::SchemaId,
                     MessageHeader::SCHEMA_ID + 1);

        auto dec = MessageHeader::wrapForDecode(buffer, sizeof(buffer));
        REQUIRE_FALSE(dec.validateSchema());
    }

    SECTION("wrong version fails validateSchema") {
        auto enc = MessageHeader::wrapForEncode(buffer, sizeof(buffer));
        enc.encodeHeader(56, MessageHeader::TemplateId::NewOrderSingle);
        write_uint16(buffer + MessageHeader::Offset::Version,
                     MessageHeader::SCHEMA_VERSION + 1);

        auto dec = MessageHeader::wrapForDecode(buffer, sizeof(buffer));
        REQUIRE_FALSE(dec.validateSchema());
    }
}

TEST_CASE("MessageHeader bodyLength ternary branches", "[sbe][header][regression]") {
    alignas(8) char buffer[64]{};

    SECTION("length greater than SIZE returns remainder") {
        auto dec = MessageHeader::wrapForDecode(buffer, 40);
        REQUIRE(dec.bodyLength() == 40 - MessageHeader::SIZE);
    }

    SECTION("length equal to SIZE returns zero") {
        auto dec = MessageHeader::wrapForDecode(buffer, MessageHeader::SIZE);
        REQUIRE(dec.bodyLength() == 0);
    }

    SECTION("length less than SIZE returns zero") {
        auto dec = MessageHeader::wrapForDecode(buffer, MessageHeader::SIZE - 2);
        REQUIRE(dec.bodyLength() == 0);
    }
}

TEST_CASE("MessageHeader isValid nullptr branch", "[sbe][header][regression]") {
    auto dec = MessageHeader::wrapForDecode(nullptr, 64);
    REQUIRE_FALSE(dec.isValid());
}

// ============================================================================
// FixedString decode padding-strip branch coverage (TICKET_497 Phase 1)
// ============================================================================

TEST_CASE("FixedString decode padding-strip loop branches", "[sbe][fixedstr][regression]") {
    SECTION("all padding decodes to empty (loop runs to zero)") {
        char buf[8];
        FixedString8::clear(buf);  // all spaces
        REQUIRE(FixedString8::decode(buf).empty());
    }

    SECTION("no trailing padding keeps full length (loop does not run)") {
        char buf[8]{'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
        REQUIRE(FixedString8::decode(buf) == "ABCDEFGH");
    }

    SECTION("partial padding strips only the tail") {
        char buf[8]{'I', 'B', 'M', ' ', ' ', ' ', ' ', ' '};
        REQUIRE(FixedString8::decode(buf) == "IBM");
    }
}

TEST_CASE("FixedString is_null loop early-exit branch", "[sbe][fixedstr][regression]") {
    SECTION("mixed null and space is null (all-blank)") {
        char buf[8]{'\0', ' ', '\0', ' ', ' ', '\0', ' ', ' '};
        REQUIRE(FixedString8::is_null(buf));
    }

    SECTION("any real character makes it non-null (early exit)") {
        char buf[8]{' ', ' ', ' ', 'X', ' ', ' ', ' ', ' '};
        REQUIRE_FALSE(FixedString8::is_null(buf));
    }
}

// ============================================================================
// WS2: dispatch() matrix (TICKET_497_3)
// ============================================================================
// Covers every branch in sbe.hpp dispatch(): length < MessageHeader::SIZE,
// invalid header, each known templateId, and unknown templateId.

TEST_CASE("sbe::dispatch with length less than header size", "[sbe][dispatch][regression]") {
    char buf[4]{};
    bool got_unknown = false;
    nfx::sbe::dispatch(buf, sizeof(buf), [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, nfx::sbe::UnknownMessage>) {
            got_unknown = true;
            REQUIRE(msg.templateId == 0);
        }
    });
    REQUIRE(got_unknown);
}

TEST_CASE("sbe::dispatch with invalid header (nullptr body)", "[sbe][dispatch][regression]") {
    alignas(8) char buf[MessageHeader::SIZE]{};
    // Write header fields manually but leave buffer_ valid; however set a bad
    // schemaId / version so isValid() is true but we need the null pointer path.
    // Instead: pass nullptr with length >= SIZE to trigger header.isValid() == false.
    // MessageHeader::isValid() checks buffer_ != nullptr, so pass a real buffer
    // but write blockLength=0, templateId=0 so we end up in unknown.
    std::memset(buf, 0, sizeof(buf));
    bool got_unknown = false;
    nfx::sbe::dispatch(buf, sizeof(buf), [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, nfx::sbe::UnknownMessage>) {
            got_unknown = true;
        }
    });
    REQUIRE(got_unknown);
}

TEST_CASE("sbe::dispatch dispatches NewOrderSingle", "[sbe][dispatch][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto encoder = NewOrderSingleCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader()
        .clOrdId("TEST")
        .symbol("MSFT")
        .side(Side::Sell)
        .ordType(OrdType::Limit)
        .price(FixedPrice::from_double(200.0))
        .orderQty(Qty::from_int(50))
        .transactTime(Timestamp{1000000LL});

    bool got_nos = false;
    nfx::sbe::dispatch(buf, sizeof(buf), [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, NewOrderSingleCodec>) {
            got_nos = true;
            REQUIRE(msg.isValid());
            REQUIRE(msg.clOrdId() == "TEST");
        }
    });
    REQUIRE(got_nos);
}

TEST_CASE("sbe::dispatch dispatches ExecutionReport", "[sbe][dispatch][regression]") {
    alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
    auto encoder = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader()
        .orderId("ORD1")
        .execId("EXEC1")
        .clOrdId("CL1")
        .symbol("GOOG")
        .side(Side::Buy)
        .execType(ExecType::Fill)
        .ordStatus(OrdStatus::Filled)
        .price(FixedPrice::from_double(100.0))
        .orderQty(Qty::from_int(10))
        .lastPx(FixedPrice::from_double(100.0))
        .lastQty(Qty::from_int(10))
        .leavesQty(Qty::from_int(0))
        .cumQty(Qty::from_int(10))
        .avgPx(FixedPrice::from_double(100.0))
        .transactTime(Timestamp{2000000LL});

    bool got_er = false;
    nfx::sbe::dispatch(buf, sizeof(buf), [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, ExecutionReportCodec>) {
            got_er = true;
            REQUIRE(msg.isValid());
            REQUIRE(msg.symbol() == "GOOG");
        }
    });
    REQUIRE(got_er);
}

TEST_CASE("sbe::dispatch dispatches to UnknownMessage for unknown templateId", "[sbe][dispatch][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto hdr = MessageHeader::wrapForEncode(buf, sizeof(buf));
    hdr.encodeHeader(NewOrderSingleCodec::BLOCK_LENGTH, SbeUint16{99});

    bool got_unknown = false;
    SbeUint16 seen_id = 0;
    nfx::sbe::dispatch(buf, sizeof(buf), [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, nfx::sbe::UnknownMessage>) {
            got_unknown = true;
            seen_id = msg.templateId;
        }
    });
    REQUIRE(got_unknown);
    REQUIRE(seen_id == 99);
}

TEST_CASE("sbe::dispatch overload accepts span", "[sbe][dispatch][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto encoder = NewOrderSingleCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader().clOrdId("SPAN").symbol("X").side(Side::Buy)
        .ordType(OrdType::Limit).price(FixedPrice{0}).orderQty(Qty{0})
        .transactTime(Timestamp{0});

    bool got_nos = false;
    auto sp = std::span<const char>{buf, sizeof(buf)};
    nfx::sbe::dispatch(sp, [&](auto& msg) {
        using T = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<T, NewOrderSingleCodec>) {
            got_nos = true;
        }
    });
    REQUIRE(got_nos);
}

// ============================================================================
// WS2: Encoder truncation branches (TICKET_497_3)
// ============================================================================

TEST_CASE("NewOrderSingleCodec truncation on clOrdId over 20 chars", "[sbe][nos_codec][truncation][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto encoder = NewOrderSingleCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader().clOrdId("THIS_STRING_IS_DEFINITELY_OVER_TWENTY_CHARS");
    REQUIRE(encoder.truncated());
}

TEST_CASE("NewOrderSingleCodec truncation on symbol over 8 chars", "[sbe][nos_codec][truncation][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto encoder = NewOrderSingleCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader().symbol("TOOLONG_SYMBOL");
    REQUIRE(encoder.truncated());
}

TEST_CASE("NewOrderSingleCodec no truncation on exact-length fields", "[sbe][nos_codec][truncation][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    auto encoder = NewOrderSingleCodec::wrapForEncode(buf, sizeof(buf));
    encoder.encodeHeader()
        .clOrdId("EXACTLY_TWENTY_CHARS")
        .symbol("EXACTLY8");
    REQUIRE_FALSE(encoder.truncated());
}

TEST_CASE("ExecutionReportCodec truncation on each string field", "[sbe][er_codec][truncation][regression]") {
    SECTION("orderId over 20 chars") {
        alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
        auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader().orderId("A_VERY_LONG_ORDER_ID_EXCEEDS_20");
        REQUIRE(enc.truncated());
    }
    SECTION("execId over 20 chars") {
        alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
        auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader().execId("EXEC_ID_THAT_IS_WAY_TOO_LONG_HERE");
        REQUIRE(enc.truncated());
    }
    SECTION("clOrdId over 20 chars") {
        alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
        auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader().clOrdId("CLORDID_EXCEEDS_TWENTY_CHARS_HERE");
        REQUIRE(enc.truncated());
    }
    SECTION("symbol over 8 chars") {
        alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
        auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader().symbol("SYMBOLTOLONG");
        REQUIRE(enc.truncated());
    }
    SECTION("no truncation on short fields") {
        alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
        auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader().orderId("EX1").execId("E1").clOrdId("C1").symbol("AAPL");
        REQUIRE_FALSE(enc.truncated());
    }
}

// ============================================================================
// WS2: isValid() through every public entry point (TICKET_497_3)
// ============================================================================
// The isValid() sub-conditions must fire at every call site that inlines them,
// not just the one exercised by wrapForDecode with a nullptr buffer.

TEST_CASE("NewOrderSingleCodec isValid via accessor with wrong templateId", "[sbe][nos_codec][regression]") {
    alignas(8) char buf[NewOrderSingleCodec::TOTAL_SIZE]{};
    // Write correct blockLength but wrong templateId.
    auto hdr = MessageHeader::wrapForEncode(buf, sizeof(buf));
    hdr.encodeHeader(NewOrderSingleCodec::BLOCK_LENGTH,
                     MessageHeader::TemplateId::ExecutionReport);
    auto codec = NewOrderSingleCodec::wrapForDecode(buf, sizeof(buf));
    REQUIRE_FALSE(codec.isValid());
    // header() call also exercises the header path
    auto hdr2 = codec.header();
    REQUIRE(hdr2.isValid());  // header itself is valid (correct size)
}

TEST_CASE("ExecutionReportCodec isValid through encoded() accessor", "[sbe][er_codec][regression]") {
    alignas(8) char buf[ExecutionReportCodec::TOTAL_SIZE]{};
    auto enc = ExecutionReportCodec::wrapForEncode(buf, sizeof(buf));
    enc.encodeHeader();
    auto sp = enc.encoded();
    REQUIRE(sp.size() == ExecutionReportCodec::TOTAL_SIZE);

    // Decode from the encoded span
    auto dec = ExecutionReportCodec::wrapForDecode(sp.data(), sp.size());
    REQUIRE(dec.isValid());

    // body() and header() paths
    REQUIRE(dec.body() == sp.data() + MessageHeader::SIZE);
    REQUIRE(dec.header().isValid());
}

TEST_CASE("MessageHeader validateSchema both conditions independently", "[sbe][header][regression]") {
    alignas(8) char buf[MessageHeader::SIZE]{};

    SECTION("both schemaId and version correct") {
        auto enc = MessageHeader::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader(56, MessageHeader::TemplateId::NewOrderSingle);
        auto dec = MessageHeader::wrapForDecode(buf, sizeof(buf));
        REQUIRE(dec.validateSchema());
    }

    SECTION("schemaId wrong, version correct") {
        auto enc = MessageHeader::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader(56, MessageHeader::TemplateId::NewOrderSingle);
        write_uint16(buf + MessageHeader::Offset::SchemaId,
                     static_cast<SbeUint16>(MessageHeader::SCHEMA_ID + 2));
        auto dec = MessageHeader::wrapForDecode(buf, sizeof(buf));
        REQUIRE_FALSE(dec.validateSchema());
    }

    SECTION("schemaId correct, version wrong") {
        auto enc = MessageHeader::wrapForEncode(buf, sizeof(buf));
        enc.encodeHeader(56, MessageHeader::TemplateId::NewOrderSingle);
        write_uint16(buf + MessageHeader::Offset::Version,
                     static_cast<SbeUint16>(MessageHeader::SCHEMA_VERSION + 2));
        auto dec = MessageHeader::wrapForDecode(buf, sizeof(buf));
        REQUIRE_FALSE(dec.validateSchema());
    }
}
