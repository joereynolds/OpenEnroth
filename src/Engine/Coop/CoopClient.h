#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    /**
     * Sends everything queued so far right away instead of on the next `update`.
     */
    void flush();

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
     * @return                          Whether joining again later may succeed where this attempt failed, e.g. because
     *                                  the host wasn't reachable or wasn't in a game yet.
     */
    [[nodiscard]] bool canRetry() const {
        return _canRetry;
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

    /**
     * Tells the host that this client's party hurt a monster. Does nothing until joined.
     *
     * @param damage                    Which monster took how much damage.
     */
    void sendActorDamage(const CoopActorDamage &damage);

    /**
     * @return                          Monster states received from the host since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopActorStates> takeActorStates();

    /**
     * @return                          Monster attacks on this client's party received since the last call, oldest
     *                                  first.
     */
    [[nodiscard]] std::vector<CoopPartyHit> takePartyHits();

    /**
     * Asks the host to put a trip to a vote. Does nothing until joined. The outcome is reported by
     * `takeTravelDecision`.
     *
     * @param destination               Where to travel.
     */
    void requestTravel(const MapDestination &destination);

    /**
     * Answers the vote returned by `pendingVote`. Does nothing if there's none.
     *
     * @param accepted                  Whether this player agrees to travel.
     */
    void answerTravel(bool accepted);

    /**
     * @return                          Vote that's waiting for this player's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopTravelVote> &pendingVote() const {
        return _pendingVote;
    }

    /**
     * @return                          Outcome of the last vote or of a turned down request, if there's one that
     *                                  hasn't been taken yet.
     */
    [[nodiscard]] std::optional<CoopTravelDecision> takeTravelDecision();

 private:
    void handleMessage(CoopMessage message);
    void fail(std::string reason);

 private:
    std::unique_ptr<NetHost> _net;
    std::string _playerName;
    NetPeerId _host = -1;
    CoopClientState _state = COOP_CLIENT_CONNECTING;
    std::string _failureReason;
    bool _canRetry = false;
    int _playerId = -1;
    Blob _saveGame;
    std::map<int, CoopPartyPose> _remoteParties;
    std::optional<CoopTravelVote> _pendingVote;
    std::optional<CoopTravelDecision> _travelDecision;
    std::vector<CoopActorStates> _actorStates;
    std::vector<CoopPartyHit> _partyHits;
};
