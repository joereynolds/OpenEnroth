#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "Library/Net/NetHost.h"

#include "Utility/Memory/Blob.h"

#include "CoopEnums.h"
#include "CoopProtocol.h"

/**
 * Client side of a co-op session. Joins a host, receives the world to start from, and keeps track of the other
 * players' parties.
 */
class CoopClient {
 public:
    /**
     * Starts connecting to a host, the outcome is reported through `state` once `update` has been called enough.
     *
     * @param address                   Host name or IP address of the host.
     * @param port                      Port the host is listening on.
     * @param playerName                Name to show to the other players.
     * @throws Exception                If the address couldn't be resolved.
     */
    CoopClient(std::string_view address, int port, std::string_view playerName);
    ~CoopClient();

    /**
     * Processes all pending network events. Should be called once per frame.
     */
    void update();

    [[nodiscard]] CoopClientState state() const {
        return _state;
    }

    /**
     * @return                          Why the session failed, empty unless `state` is `COOP_CLIENT_FAILED`.
     */
    [[nodiscard]] const std::string &failureReason() const {
        return _failureReason;
    }

    /**
     * @return                          Player id the host assigned, -1 until joined.
     */
    [[nodiscard]] int playerId() const {
        return _playerId;
    }

    /**
     * Hands over the save game the host sent on joining. Can be called only once, it's empty afterwards.
     *
     * @return                          Host's game serialized as a save file, empty if not joined yet.
     */
    [[nodiscard]] Blob takeSaveGame();

    /**
     * Sends this client's party pose to the host. Does nothing until joined.
     *
     * @param pose                      Pose of the local party, its player id is ignored.
     */
    void sendLocalPose(const CoopPartyPose &pose);

    /**
     * @return                          Last known poses of all other players' parties, by player id.
     */
    [[nodiscard]] const std::map<int, CoopPartyPose> &remoteParties() const {
        return _remoteParties;
    }

 private:
    void handleMessage(CoopMessage message);
    void fail(std::string reason);

 private:
    std::unique_ptr<NetHost> _net;
    std::string _playerName;
    NetPeerId _host = -1;
    CoopClientState _state = COOP_CLIENT_CONNECTING;
    std::string _failureReason;
    int _playerId = -1;
    Blob _saveGame;
    std::map<int, CoopPartyPose> _remoteParties;
};
