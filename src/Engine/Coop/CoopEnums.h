#pragma once

enum class CoopClientState {
    COOP_CLIENT_CONNECTING, // Waiting for the connection and the host's welcome.
    COOP_CLIENT_IN_LOBBY, // Accepted into the lobby of a game the host hasn't started yet, waiting for the welcome.
    COOP_CLIENT_JOINED,
    COOP_CLIENT_FAILED, // Rejected, couldn't connect, or disconnected. See `CoopClient::failureReason`.
};
using enum CoopClientState;

enum class CoopVoteKind {
    COOP_VOTE_TRAVEL, // Move the group to another map.
    COOP_VOTE_REST, // Rest and heal for eight hours.
};
using enum CoopVoteKind;
