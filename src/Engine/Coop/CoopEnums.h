#pragma once

enum class CoopClientState {
    COOP_CLIENT_CONNECTING, // Waiting for the connection and the host's welcome.
    COOP_CLIENT_JOINED,
    COOP_CLIENT_FAILED, // Rejected, couldn't connect, or disconnected. See `CoopClient::failureReason`.
};
using enum CoopClientState;
