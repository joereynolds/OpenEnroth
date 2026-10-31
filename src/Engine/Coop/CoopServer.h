#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Library/Net/NetHost.h"

#include "Utility/Memory/Blob.h"

#include "CoopProtocol.h"

/**
 * Host side of a co-op session. Accepts clients, hands them the world to start from, and relays party poses
 * between all players.
 *
 * The host itself is player `HOST_PLAYER_ID`, clients get increasing ids that are never reused.
 */
class CoopServer {
 public:
    static constexpr int HOST_PLAYER_ID = 0;

    /**
     * @param port                      Port to listen on, zero picks a free one.
     * @param maxClients                Maximal number of simultaneously connected clients.
     * @param saveGameProvider          Called whenever a client says hello, returns the current game serialized
     *                                  as a save file, or an empty blob if the host isn't in a game. The client is
     *                                  then asked to try again later.
     * @param hostName                  Name of the host's player, shown to the clients.
     * @throws Exception                If the port couldn't be bound.
     */
    CoopServer(int port, int maxClients, std::function<Blob()> saveGameProvider, std::string hostName = "Host");
    ~CoopServer();

    /**
     * @return                          Port this server is listening on.
     */
    [[nodiscard]] int port() const;

    /**
     * Processes all pending network events. Should be called once per frame.
     */
    void update();

    /**
     * Sends everything queued so far right away instead of on the next `update`.
     */
    void flush();

    /**
     * Sends the host party's pose to all clients.
     *
     * @param pose                      Pose of the host party, its player id is ignored.
     */
    void sendLocalPose(const CoopPartyPose &pose);

    /**
     * @return                          Last known poses of all other players' parties, by player id.
     */
    [[nodiscard]] const std::map<int, CoopPartyPose> &remoteParties() const {
        return _remoteParties;
    }

    /**
     * Sends the state of the monsters on the host's map to all clients.
     *
     * @param states                    Monster states to send.
     */
    void sendActorStates(const CoopActorStates &states);

    /**
     * Tells a client that a monster attacked its party. Does nothing if the player has left.
     *
     * @param playerId                  Player whose party was attacked.
     * @param hit                       Who attacked, and how.
     */
    void sendPartyHit(int playerId, const CoopPartyHit &hit);

    /**
     * Sends a quest bit change to every client except the one it came from.
     *
     * @param change                    The change, with the id of the player whose game made it.
     */
    void sendQuestBit(const CoopQuestBit &change);

    /**
     * @return                          Quest bit changes clients sent since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopQuestBit> takeQuestBits();

    /**
     * Sends quest rewards to every client except the one whose game gave them out.
     *
     * @param rewards                   The rewards, with the id of the player whose game gave them out.
     */
    void sendQuestRewards(const CoopQuestRewards &rewards);

    /**
     * @return                          Quest rewards clients sent since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopQuestRewards> takeQuestRewards();

    /**
     * Sends a chest's contents to every client except the one whose game changed them.
     *
     * @param chest                     The chest, with the id of the player whose game changed it.
     */
    void sendChest(const CoopChest &chest);

    /**
     * @return                          Chests clients changed since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopChest> takeChests();

    /**
     * Tells every client except the one whose party took it that a ground item is gone.
     *
     * @param taken                     The item, with the id of the player whose party took it.
     */
    void sendGroundItemTaken(const CoopGroundItemTaken &taken);

    /**
     * @return                          Ground items clients' parties took since the last call, oldest first.
     */
    [[nodiscard]] std::vector<CoopGroundItemTaken> takeGroundItemsTaken();

    /**
     * Sends the host's game time to all clients.
     *
     * @param clock                     The host's game time.
     */
    void sendClock(const CoopClock &clock);

    /**
     * Gives a client what its party found on a corpse. Does nothing if the player has left.
     *
     * @param playerId                  Player whose party looted the corpse.
     * @param loot                      What the party found.
     */
    void sendLoot(int playerId, const CoopLoot &loot);

    /**
     * @return                          Corpses clients' parties want to loot, received since the last call, oldest
     *                                  first.
     */
    [[nodiscard]] std::vector<CoopLootRequest> takeLootRequests();

    /**
     * @return                          Damage that clients' parties dealt to monsters since the last call, oldest
     *                                  first.
     */
    [[nodiscard]] std::vector<CoopActorDamage> takeActorDamage();

    /**
     * Proposes something the whole group has to agree on, on behalf of the host. Everyone else is asked, and the
     * outcome is reported by `takeVoteDecision`. With nobody else around it's approved right away.
     *
     * @param kind                      What to vote on.
     * @param destination               Where to travel, for a travel vote.
     */
    void requestVote(CoopVoteKind kind, const MapDestination &destination = {});

    /**
     * Answers the vote returned by `pendingVote` on behalf of the host. Does nothing if there's none.
     *
     * @param accepted                  Whether the host agrees to travel.
     */
    void answerVote(bool accepted);

    /**
     * @return                          Vote that's waiting for the host's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopVote> &pendingVote() const {
        return _hostVote;
    }

    /**
     * @return                          Outcome of the last vote or of a turned down request, if there's one the host
     *                                  hasn't taken yet.
     */
    [[nodiscard]] std::optional<CoopVoteDecision> takeVoteDecision();

 private:
    struct Client {
        int playerId = -1; // -1 until the client's hello is accepted.
        std::string playerName;
    };

    void handleMessage(NetPeerId peer, CoopMessage message);
    void handleHello(NetPeerId peer, const CoopHello &hello);
    void handlePartyPose(NetPeerId peer, CoopPartyPose pose);
    void handleDisconnect(NetPeerId peer);
    void startVote(int proposerId, NetPeerId proposerPeer, CoopVoteKind kind, const MapDestination &destination);
    void recordAnswer(int playerId, bool accepted);
    void finishVote(bool approved, std::string declinedBy);
    [[nodiscard]] std::string playerName(int playerId) const;
    void reject(NetPeerId peer, CoopReject message);
    void send(NetPeerId peer, NetChannel channel, CoopMessage message);
    void sendToJoined(NetChannel channel, CoopMessage message, NetPeerId except = -1);

 private:
    std::unique_ptr<NetHost> _net;
    std::function<Blob()> _saveGameProvider;
    std::unordered_map<NetPeerId, Client> _clients;
    int _nextPlayerId = HOST_PLAYER_ID + 1;
    std::optional<CoopPartyPose> _localPose;
    std::map<int, CoopPartyPose> _remoteParties;
    std::string _hostName;

    struct Vote {
        int id = -1;
        CoopVoteKind kind = COOP_VOTE_TRAVEL;
        MapDestination destination;
        std::set<int> waitingFor; // Player ids that haven't answered yet.
    };
    std::optional<Vote> _vote;
    int _nextVoteId = 1;
    std::optional<CoopVote> _hostVote;
    std::optional<CoopVoteDecision> _hostDecision;
    std::vector<CoopActorDamage> _actorDamage;
    std::vector<CoopLootRequest> _lootRequests;
    std::vector<CoopQuestBit> _questBits;
    std::vector<CoopQuestRewards> _questRewards;
    std::vector<CoopChest> _chests;
    std::vector<CoopGroundItemTaken> _groundItemsTaken;
};
