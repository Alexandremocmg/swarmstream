// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#pragma once

#include <future>
#include <string>
#include <string_view>
#include <vector>
#include "swarmstream/types.hpp"

namespace swarmstream {

// Response payload returned upon completion of a piece download request.
struct PieceResponse {
    PieceId piece{};
    std::vector<uint8_t> data{};
    SourceErrorCode error{SourceErrorCode::Success};
    std::string error_message{};

    [[nodiscard]] bool ok() const noexcept {
        return error == SourceErrorCode::Success;
    }
};

// Handle to an ongoing piece request containing the unique ID and the async future.
struct RequestTicket {
    RequestId request_id{kInvalidRequestId};
    std::future<PieceResponse> future{};
};

// IContentSource: Interface connecting the deadline scheduler to piece sources (CDN, P2P swarm, etc.)
// Minimal interface without any libtorrent or network protocol details (see docs/02-arquitetura.md).
class IContentSource {
public:
    virtual ~IContentSource() = default;

    // Submits an asynchronous request for a piece with an explicit deadline.
    // The scheduler will re-prioritize or hedge requests based on deadlines.
    [[nodiscard]] virtual RequestTicket request(
        const PieceId& piece,
        SteadyTimePoint deadline) = 0;

    // Cancels an in-flight request.
    // Returns true if the request was successfully canceled before delivery.
    virtual bool cancel(RequestId request_id) = 0;

    // Returns runtime operational statistics for this source.
    [[nodiscard]] virtual SourceStats stats() const = 0;

    // Identifier name of the source (e.g. "cdn", "p2p", "mock").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

} // namespace swarmstream
