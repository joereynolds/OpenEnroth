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
     * Tells the host that this client's game changed a quest bit. Does nothing until joined.
     *
     * @param change                    The change.
     */
    void sendQuestBit(const CoopQuestBit &change);

    /**
     * @return                          Quest bit changes from the other players since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopQuestBit> takeQuestBits();

    /**
     * Tells the host what this client's game gave out for a quest. Does nothing until joined.
     *
     * @param rewards                   The rewards.
     */
    void sendQuestRewards(const CoopQuestRewards &rewards);

    /**
     * @return                          Quest rewards from the other players since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopQuestRewards> takeQuestRewards();

    /**
     * Tells the host that this client's game changed a chest. Does nothing until joined.
     *
     * @param chest                     The chest.
     */
    void sendChest(const CoopChest &chest);

    /**
     * @return                          Chests the other players changed since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopChest> takeChests();

    /**
     * Tells the host that this client's party took an item from the ground. Does nothing until joined.
     *
     * @param taken                     The item.
     */
    void sendGroundItemTaken(const CoopGroundItemTaken &taken);

    /**
     * @return                          Ground items the other players' parties took since the last call, oldest
     *                                  first.
     */
    [[nodiscard]] std::vector<CoopGroundItemTaken> takeGroundItemsTaken();

    /**
     * @return                          The newest game time the host sent since the last call, if any.
     */
    [[nodiscard]] std::optional<CoopClock> takeClock();

    /**
     * Tells the host that this client's party put a spell effect on a monster. Does nothing until joined.
     *
     * @param buff                      The monster and the effect.
     */
    void sendActorBuff(const CoopActorBuff &buff);

    /**
     * Tells the host that this client's party dropped an item. Does nothing until joined.
     *
     * @param dropped                   The item and how it was thrown.
     */
    void sendItemDropped(const CoopItemDropped &dropped);

    /**
     * @return                          Items the other players' parties dropped since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopItemDropped> takeItemsDropped();

    /**
     * Tells the host that this client's party launched a projectile or spell effect. Does nothing until joined.
     *
     * @param launched                  The sprite as it was launched.
     */
    void sendSpriteLaunched(const CoopSpriteLaunched &launched);

    /**
     * @return                          Sprites the other players' parties launched since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopSpriteLaunched> takeSpritesLaunched();

    /**
     * Asks the host for what's on a corpse. Does nothing until joined.
     *
     * @param request                   Which corpse to loot.
     */
    void sendLootRequest(const CoopLootRequest &request);

    /**
     * @return                          Loot the host handed to this client's party since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopLoot> takeLoot();

    /**
     * @return                          Monster attacks on this client's party received since the last call, oldest
     *                                  first.
     */
    [[nodiscard]] std::vector<CoopPartyHit> takePartyHits();

    /**
     * Asks the host to put something the whole group has to agree on to a vote. Does nothing until joined. The
     * outcome is reported by `takeVoteDecision`.
     *
     * @param kind                      What to vote on.
     * @param destination               Where to travel, for a travel vote.
     */
    void requestVote(CoopVoteKind kind, const MapDestination &destination = {});

    /**
     * Answers the vote returned by `pendingVote`. Does nothing if there's none.
     *
     * @param accepted                  Whether this player agrees to travel.
     */
    void answerVote(bool accepted);

    /**
     * @return                          Vote that's waiting for this player's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopVote> &pendingVote() const {
        return _pendingVote;
    }

    /**
     * @return                          Outcome of the last vote or of a turned down request, if there's one that
     *                                  hasn't been taken yet.
     */
    [[nodiscard]] std::optional<CoopVoteDecision> takeVoteDecision();

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
    std::optional<CoopVote> _pendingVote;
    std::optional<CoopVoteDecision> _voteDecision;
    std::vector<CoopActorStates> _actorStates;
    std::vector<CoopPartyHit> _partyHits;
    std::vector<CoopLoot> _loot;
    std::vector<CoopQuestBit> _questBits;
    std::vector<CoopQuestRewards> _questRewards;
    std::vector<CoopChest> _chests;
    std::vector<CoopGroundItemTaken> _groundItemsTaken;
    std::optional<CoopClock> _clock;
    std::vector<CoopItemDropped> _itemsDropped;
    std::vector<CoopSpriteLaunched> _spritesLaunched;
};
