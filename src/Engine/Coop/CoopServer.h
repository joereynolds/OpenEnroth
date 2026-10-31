#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

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
     * @param saveGameProvider          Called whenever a client is accepted, returns the current game serialized
     *                                  as a save file.
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
     * Proposes that the group travels, on behalf of the host. Everyone else is asked, and the outcome is reported by
     * `takeTravelDecision`. With nobody else around the trip is approved right away.
     *
     * @param destination               Where to travel.
     */
    void requestTravel(const MapDestination &destination);

    /**
     * Answers the vote returned by `pendingVote` on behalf of the host. Does nothing if there's none.
     *
     * @param accepted                  Whether the host agrees to travel.
     */
    void answerTravel(bool accepted);

    /**
     * @return                          Vote that's waiting for the host's answer, if any.
     */
    [[nodiscard]] const std::optional<CoopTravelVote> &pendingVote() const {
        return _hostVote;
    }

    /**
     * @return                          Outcome of the last vote or of a turned down request, if there's one the host
     *                                  hasn't taken yet.
     */
    [[nodiscard]] std::optional<CoopTravelDecision> takeTravelDecision();

 private:
    struct Client {
        int playerId = -1; // -1 until the client's hello is accepted.
        std::string playerName;
    };

    void handleMessage(NetPeerId peer, CoopMessage message);
    void handleHello(NetPeerId peer, const CoopHello &hello);
    void handlePartyPose(NetPeerId peer, CoopPartyPose pose);
    void handleDisconnect(NetPeerId peer);
    void startVote(int proposerId, NetPeerId proposerPeer, const MapDestination &destination);
    void recordAnswer(int playerId, bool accepted);
    void finishVote(bool approved, std::string declinedBy);
    [[nodiscard]] std::string playerName(int playerId) const;
    void reject(NetPeerId peer, std::string reason);
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
        MapDestination destination;
        std::set<int> waitingFor; // Player ids that haven't answered yet.
    };
    std::optional<Vote> _vote;
    int _nextVoteId = 1;
    std::optional<CoopTravelVote> _hostVote;
    std::optional<CoopTravelDecision> _hostDecision;
};
