#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "Utility/Memory/Blob.h"

#include "NetEvent.h"

struct _ENetHost;

/**
 * One end of a network connection, either a server that accepts peers or a client with a single peer.
 *
 * Nothing happens in the background. All sending and receiving is done inside `poll`, so it has to be called
 * regularly, e.g. once per frame until it returns `std::nullopt`.
 */
class NetHost {
 public:
    ~NetHost();

    /**
     * @param port                      Port to listen on, zero picks a free one.
     * @param maxPeers                  Maximal number of simultaneously connected peers.
     * @return                          Server host.
     * @throws Exception                If the port couldn't be bound.
     */
    [[nodiscard]] static std::unique_ptr<NetHost> listen(int port, int maxPeers);

    /**
     * Starts connecting to a server. The connection is established asynchronously, a `NET_EVENT_CONNECTED` or a
     * `NET_EVENT_DISCONNECTED` event is reported by `poll` once it succeeds or fails.
     *
     * @param address                   Host name or IP address of the server.
     * @param port                      Server port.
     * @return                          Client host.
     * @throws Exception                If the address couldn't be resolved.
     */
    [[nodiscard]] static std::unique_ptr<NetHost> connect(std::string_view address, int port);

    /**
     * @return                          Port this server is listening on, zero for a client.
     */
    [[nodiscard]] int port() const;

    /**
     * Sends queued packets, receives incoming ones, and returns the next event.
     *
     * @param timeoutMs                 How long to wait for an event, zero returns right away.
     * @return                          Next event, or `std::nullopt` if there is none.
     */
    [[nodiscard]] std::optional<NetEvent> poll(int timeoutMs = 0);

    /**
     * Queues a message for sending. It is sent on the next `poll` or `flush`.
     *
     * @param peer                      Connected peer to send to.
     * @param channel                   Channel to send on.
     * @param data                      Message payload.
     */
    void send(NetPeerId peer, NetChannel channel, const Blob &data);

    /**
     * Queues a message for sending to all connected peers.
     *
     * @param channel                   Channel to send on.
     * @param data                      Message payload.
     */
    void broadcast(NetChannel channel, const Blob &data);

    /**
     * Starts a graceful disconnect. Messages already queued for this peer are still delivered, and a
     * `NET_EVENT_DISCONNECTED` event for this peer is reported by `poll` once the peer acknowledges the disconnect.
     *
     * @param peer                      Connected peer to disconnect.
     */
    void disconnect(NetPeerId peer);

    /**
     * Sends all queued packets right away.
     */
    void flush();

 private:
    explicit NetHost(_ENetHost *host);

 private:
    _ENetHost *_host = nullptr;
};
