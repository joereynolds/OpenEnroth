#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
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
     * @throws Exception                If the port couldn't be bound.
     */
    CoopServer(int port, int maxClients, std::function<Blob()> saveGameProvider);
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

 private:
    struct Client {
        int playerId = -1; // -1 until the client's hello is accepted.
        std::string playerName;
    };

    void handleMessage(NetPeerId peer, CoopMessage message);
    void handleHello(NetPeerId peer, const CoopHello &hello);
    void handlePartyPose(NetPeerId peer, CoopPartyPose pose);
    void handleDisconnect(NetPeerId peer);
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
};
