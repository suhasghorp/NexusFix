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
#include "nexusfix/messages/common/header.hpp"
#include "nexusfix/messages/common/trailer.hpp"

namespace nfx::fix43 {

// ============================================================================
// FIX 4.3 MarketDataRequest Message (MsgType = V)
// ============================================================================

/// FIX 4.3 MarketDataRequest message (35=V)
/// Introduced in FIX 4.3 for market data subscription
struct MarketDataRequest {
    static constexpr char MSG_TYPE = 'V';
    static constexpr std::string_view BEGIN_STRING = fix::FIX_4_3;

    FixHeader header;
    std::string_view md_req_id;              // Tag 262 - Required
    SubscriptionRequestType subscription_type; // Tag 263 - Required
    int market_depth;                         // Tag 264 - Required
    MDUpdateType md_update_type;              // Tag 265 - Conditional
    bool aggregated_book;                     // Tag 266 - Optional

    size_t no_md_entry_types;                 // Tag 267
    size_t no_related_sym;                    // Tag 146

    std::span<const char> raw_data;

    constexpr MarketDataRequest() noexcept
        : header{}
        , md_req_id{}
        , subscription_type{SubscriptionRequestType::Snapshot}
        , market_depth{0}
        , md_update_type{MDUpdateType::FullRefresh}
        , aggregated_book{true}
        , no_md_entry_types{0}
        , no_related_sym{0}
        , raw_data{} {}

    [[nodiscard]] constexpr std::span<const char> raw() const noexcept { return raw_data; }
    [[nodiscard]] constexpr uint32_t msg_seq_num() const noexcept { return header.msg_seq_num; }
    [[nodiscard]] constexpr std::string_view sender_comp_id() const noexcept { return header.sender_comp_id; }
    [[nodiscard]] constexpr std::string_view target_comp_id() const noexcept { return header.target_comp_id; }
    [[nodiscard]] constexpr std::string_view sending_time() const noexcept { return header.sending_time; }

    [[nodiscard]] constexpr bool is_snapshot() const noexcept {
        return subscription_type == SubscriptionRequestType::Snapshot;
    }

    [[nodiscard]] constexpr bool is_subscribe() const noexcept {
        return subscription_type == SubscriptionRequestType::SnapshotPlusUpdates;
    }

    [[nodiscard]] constexpr bool is_unsubscribe() const noexcept {
        return subscription_type == SubscriptionRequestType::DisablePreviousSnapshot;
    }

    // ========================================================================
    // Parsing
    // ========================================================================

    [[nodiscard]] static ParseResult<MarketDataRequest> from_buffer(
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

        MarketDataRequest msg;
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

        // Parse required body fields
        msg.md_req_id = p.get_string(tag::MDReqID::value);
        if (msg.md_req_id.empty()) {
            return std::unexpected{ParseError{ParseErrorCode::MissingRequiredField, tag::MDReqID::value}};
        }

        char sub_type_char = p.get_char(tag::SubscriptionRequestType::value);
        if (sub_type_char == '\0') {
            return std::unexpected{ParseError{ParseErrorCode::MissingRequiredField, tag::SubscriptionRequestType::value}};
        }
        msg.subscription_type = static_cast<SubscriptionRequestType>(sub_type_char);

        if (auto v = p.get_int(tag::MarketDepth::value)) {
            msg.market_depth = static_cast<int>(*v);
        }

        if (auto v = p.get_int(tag::MDUpdateType::value)) {
            msg.md_update_type = static_cast<MDUpdateType>(*v);
        }

        if (char c = p.get_char(tag::AggregatedBook::value); c != '\0') {
            msg.aggregated_book = (c == 'Y');
        }

        if (auto v = p.get_int(tag::NoMDEntryTypes::value)) {
            msg.no_md_entry_types = static_cast<size_t>(*v);
        }

        if (auto v = p.get_int(tag::NoRelatedSym::value)) {
            msg.no_related_sym = static_cast<size_t>(*v);
        }

        return msg;
    }

    // ========================================================================
    // Builder
    // ========================================================================

    class Builder {
    public:
        Builder& sender_comp_id(std::string_view v) noexcept { sender_comp_id_ = v; return *this; }
        Builder& target_comp_id(std::string_view v) noexcept { target_comp_id_ = v; return *this; }
        Builder& msg_seq_num(uint32_t v) noexcept { msg_seq_num_ = v; return *this; }
        Builder& sending_time(std::string_view v) noexcept { sending_time_ = v; return *this; }
        Builder& md_req_id(std::string_view v) noexcept { md_req_id_ = v; return *this; }
        Builder& subscription_type(SubscriptionRequestType v) noexcept { subscription_type_ = v; return *this; }
        Builder& market_depth(int v) noexcept { market_depth_ = v; return *this; }
        Builder& md_update_type(MDUpdateType v) noexcept { md_update_type_ = v; return *this; }
        Builder& aggregated_book(bool v) noexcept { aggregated_book_ = v; return *this; }

        Builder& add_entry_type(MDEntryType type) noexcept {
            if (entry_type_count_ < MAX_ENTRY_TYPES) {
                entry_types_[entry_type_count_++] = type;
            }
            return *this;
        }

        Builder& add_symbol(std::string_view symbol) noexcept {
            if (symbol_count_ < MAX_SYMBOLS) {
                symbols_[symbol_count_++] = symbol;
            }
            return *this;
        }

        [[nodiscard]] std::span<const char> build(MessageAssembler& asm_) const noexcept {
            asm_.start(fix::FIX_4_3)
                .field(tag::MsgType::value, MSG_TYPE)
                .field(tag::SenderCompID::value, sender_comp_id_)
                .field(tag::TargetCompID::value, target_comp_id_)
                .field(tag::MsgSeqNum::value, static_cast<int64_t>(msg_seq_num_))
                .field(tag::SendingTime::value, sending_time_)
                .field(tag::MDReqID::value, md_req_id_)
                .field(tag::SubscriptionRequestType::value, static_cast<char>(subscription_type_))
                .field(tag::MarketDepth::value, static_cast<int64_t>(market_depth_));

            if (subscription_type_ == SubscriptionRequestType::SnapshotPlusUpdates) {
                asm_.field(tag::MDUpdateType::value, static_cast<int64_t>(std::to_underlying(md_update_type_)));
            }

            asm_.field(tag::AggregatedBook::value, aggregated_book_ ? 'Y' : 'N');

            if (entry_type_count_ > 0) {
                asm_.field(tag::NoMDEntryTypes::value, static_cast<int64_t>(entry_type_count_));
                for (size_t i = 0; i < entry_type_count_; ++i) {
                    asm_.field(tag::MDEntryType::value, static_cast<char>(entry_types_[i]));
                }
            }

            if (symbol_count_ > 0) {
                asm_.field(tag::NoRelatedSym::value, static_cast<int64_t>(symbol_count_));
                for (size_t i = 0; i < symbol_count_; ++i) {
                    asm_.field(tag::Symbol::value, symbols_[i]);
                }
            }

            return asm_.finish();
        }

    private:
        static constexpr size_t MAX_ENTRY_TYPES = 16;
        static constexpr size_t MAX_SYMBOLS = 64;

        std::string_view sender_comp_id_;
        std::string_view target_comp_id_;
        uint32_t msg_seq_num_{1};
        std::string_view sending_time_;
        std::string_view md_req_id_;
        SubscriptionRequestType subscription_type_{SubscriptionRequestType::SnapshotPlusUpdates};
        int market_depth_{0};
        MDUpdateType md_update_type_{MDUpdateType::IncrementalRefresh};
        bool aggregated_book_{true};

        std::array<MDEntryType, MAX_ENTRY_TYPES> entry_types_;
        size_t entry_type_count_{0};

        std::array<std::string_view, MAX_SYMBOLS> symbols_;
        size_t symbol_count_{0};
    };
};

} // namespace nfx::fix43
