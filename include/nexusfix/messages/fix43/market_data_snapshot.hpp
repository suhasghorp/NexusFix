#pragma once

#include <array>
#include <span>
#include <string_view>

#include "nexusfix/types/tag.hpp"
#include "nexusfix/types/field_types.hpp"
#include "nexusfix/types/market_data_types.hpp"
#include "nexusfix/types/error.hpp"
#include "nexusfix/interfaces/i_message.hpp"
#include "nexusfix/parser/runtime_parser.hpp"
#include "nexusfix/parser/repeating_group.hpp"
#include "nexusfix/messages/common/header.hpp"
#include "nexusfix/messages/common/trailer.hpp"

namespace nfx::fix43 {

// ============================================================================
// FIX 4.3 MarketDataSnapshotFullRefresh Message (MsgType = W)
// ============================================================================

/// FIX 4.3 MarketDataSnapshotFullRefresh message (35=W)
/// Introduced in FIX 4.3 for full market data snapshots
struct MarketDataSnapshotFullRefresh {
    static constexpr char MSG_TYPE = 'W';
    static constexpr std::string_view BEGIN_STRING = fix::FIX_4_3;

    FixHeader header;
    std::string_view md_req_id;       // Tag 262 - Conditional
    std::string_view symbol;          // Tag 55 - Required
    std::string_view security_id;     // Tag 48 - Optional
    std::string_view security_exchange; // Tag 207 - Optional
    size_t no_md_entries;             // Tag 268 - Required

    std::span<const char> raw_data;

    constexpr MarketDataSnapshotFullRefresh() noexcept
        : header{}
        , md_req_id{}
        , symbol{}
        , security_id{}
        , security_exchange{}
        , no_md_entries{0}
        , raw_data{} {}

    [[nodiscard]] constexpr std::span<const char> raw() const noexcept { return raw_data; }
    [[nodiscard]] constexpr uint32_t msg_seq_num() const noexcept { return header.msg_seq_num; }
    [[nodiscard]] constexpr std::string_view sender_comp_id() const noexcept { return header.sender_comp_id; }
    [[nodiscard]] constexpr std::string_view target_comp_id() const noexcept { return header.target_comp_id; }
    [[nodiscard]] constexpr std::string_view sending_time() const noexcept { return header.sending_time; }

    [[nodiscard]] parser::MDEntryIterator entries() const noexcept {
        return parser::MDEntryIterator{raw_data, no_md_entries};
    }

    [[nodiscard]] constexpr size_t entry_count() const noexcept {
        return no_md_entries;
    }

    // ========================================================================
    // Parsing
    // ========================================================================

    [[nodiscard]] static ParseResult<MarketDataSnapshotFullRefresh> from_buffer(
        std::span<const char> buffer) noexcept
    {
        auto parsed = IndexedParser::parse(buffer);
        if (!parsed.has_value()) {
            return std::unexpected{parsed.error()};
        }

        auto& p = *parsed;

        if (p.msg_type() != MSG_TYPE) {
            return std::unexpected{ParseError{ParseErrorCode::InvalidMsgType}};
        }

        MarketDataSnapshotFullRefresh msg;
        msg.raw_data = buffer;

        // Parse header
        msg.header.begin_string = p.get_string(tag::BeginString::value);
        if (auto v = p.get_int(tag::BodyLength::value)) {
            msg.header.body_length = static_cast<int>(*v);
        }
        msg.header.msg_type = p.msg_type();
        msg.header.sender_comp_id = p.sender_comp_id();
        msg.header.target_comp_id = p.target_comp_id();
        msg.header.msg_seq_num = p.msg_seq_num();
        msg.header.sending_time = p.sending_time();

        // Parse body fields
        msg.md_req_id = p.get_string(tag::MDReqID::value);
        msg.symbol = p.get_string(tag::Symbol::value);

        if (msg.symbol.empty()) {
            return std::unexpected{ParseError{ParseErrorCode::MissingRequiredField, tag::Symbol::value}};
        }

        msg.security_id = p.get_string(tag::SecurityID::value);
        msg.security_exchange = p.get_string(tag::SecurityExchange::value);

        if (auto v = p.get_int(tag::NoMDEntries::value)) {
            msg.no_md_entries = static_cast<size_t>(*v);
        }

        return msg;
    }

    // ========================================================================
    // Building
    // ========================================================================

    struct MDEntry {
        MDEntryType entry_type{MDEntryType::Bid};
        FixedPrice entry_px;
        Qty entry_size;
    };

    class Builder {
    public:
        Builder& sender_comp_id(std::string_view v) noexcept { sender_comp_id_ = v; return *this; }
        Builder& target_comp_id(std::string_view v) noexcept { target_comp_id_ = v; return *this; }
        Builder& msg_seq_num(uint32_t v) noexcept { msg_seq_num_ = v; return *this; }
        Builder& sending_time(std::string_view v) noexcept { sending_time_ = v; return *this; }
        Builder& md_req_id(std::string_view v) noexcept { md_req_id_ = v; return *this; }
        Builder& symbol(std::string_view v) noexcept { symbol_ = v; return *this; }
        Builder& security_id(std::string_view v) noexcept { security_id_ = v; return *this; }
        Builder& security_exchange(std::string_view v) noexcept { security_exchange_ = v; return *this; }

        Builder& add_entry(MDEntryType type, FixedPrice px, Qty size) noexcept {
            if (entry_count_ < MAX_ENTRIES) {
                entries_[entry_count_++] = MDEntry{type, px, size};
            }
            return *this;
        }

        [[nodiscard]] std::span<const char> build(MessageAssembler& asm_) const noexcept {
            asm_.start(fix::FIX_4_3)
                .field(tag::MsgType::value, MSG_TYPE)
                .field(tag::SenderCompID::value, sender_comp_id_)
                .field(tag::TargetCompID::value, target_comp_id_)
                .field(tag::MsgSeqNum::value, static_cast<int64_t>(msg_seq_num_))
                .field(tag::SendingTime::value, sending_time_);

            if (!md_req_id_.empty()) {
                asm_.field(tag::MDReqID::value, md_req_id_);
            }

            asm_.field(tag::Symbol::value, symbol_);

            if (!security_id_.empty()) {
                asm_.field(tag::SecurityID::value, security_id_);
            }

            if (!security_exchange_.empty()) {
                asm_.field(tag::SecurityExchange::value, security_exchange_);
            }

            if (entry_count_ > 0) {
                asm_.field(tag::NoMDEntries::value, static_cast<int64_t>(entry_count_));
                for (size_t i = 0; i < entry_count_; ++i) {
                    asm_.field(tag::MDEntryType::value, static_cast<char>(entries_[i].entry_type));
                    asm_.field(tag::MDEntryPx::value, entries_[i].entry_px);
                    asm_.field(tag::MDEntrySize::value, static_cast<int64_t>(entries_[i].entry_size.whole()));
                }
            }

            return asm_.finish();
        }

    private:
        static constexpr size_t MAX_ENTRIES = 64;

        std::string_view sender_comp_id_;
        std::string_view target_comp_id_;
        uint32_t msg_seq_num_{1};
        std::string_view sending_time_;
        std::string_view md_req_id_;
        std::string_view symbol_;
        std::string_view security_id_;
        std::string_view security_exchange_;

        std::array<MDEntry, MAX_ENTRIES> entries_;
        size_t entry_count_{0};
    };
};

} // namespace nfx::fix43
