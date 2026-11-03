#include "NetHost.h"

#include <enet/enet.h>

#include <cassert>
#include <memory>
#include <string>

#include "Utility/Exception.h"

static constexpr int CHANNEL_COUNT = 2;

static void ensureEnetInitialized() {
    struct EnetInitializer {
        EnetInitializer() {
            if (enet_initialize() != 0)
                throw Exception("Could not initialize the network library");
        }

        ~EnetInitializer() {
            enet_deinitialize();
        }
    };

    static EnetInitializer initializer;
}

static enet_uint8 channelId(NetChannel channel) {
    return channel == NET_CHANNEL_RELIABLE ? 0 : 1;
}

static enet_uint32 packetFlags(NetChannel channel) {
    return channel == NET_CHANNEL_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0;
}

static ENetPacket *createPacket(NetChannel channel, const Blob &data) {
    ENetPacket *packet = enet_packet_create(data.data(), data.size(), packetFlags(channel));
    if (!packet)
        throw Exception("Could not allocate a network packet of {} bytes", data.size());
    return packet;
}

static void setPeerTimeout(ENetPeer *peer, int timeoutMs) {
    // The limit counts retransmits, the minimum and maximum are times. Equal times make the drop happen right
    // when the time is up, whatever the retransmit count.
    enet_peer_timeout(peer, 0, timeoutMs, timeoutMs);
}

NetHost::NetHost(_ENetHost *host, int timeoutMs) : _host(host), _timeoutMs(timeoutMs) {}

NetHost::~NetHost() {
    for (size_t i = 0; i < _host->peerCount; i++)
        enet_peer_disconnect_now(&_host->peers[i], 0); // Tells the other side right away instead of letting it time out.
    enet_host_destroy(_host);
}

std::unique_ptr<NetHost> NetHost::listen(int port, int maxPeers, int timeoutMs) {
    assert(timeoutMs > 0);
    assert(port >= 0 && port <= 65535);
    assert(maxPeers > 0 && maxPeers <= ENET_PROTOCOL_MAXIMUM_PEER_ID);

    ensureEnetInitialized();

    ENetAddress address = {};
    address.host = ENET_HOST_ANY;
    address.port = static_cast<enet_uint16>(port);

    ENetHost *host = enet_host_create(&address, maxPeers, CHANNEL_COUNT, 0, 0);
    if (!host)
        throw Exception("Could not listen on port {}", port);
    return std::unique_ptr<NetHost>(new NetHost(host, timeoutMs));
}

std::unique_ptr<NetHost> NetHost::connect(std::string_view address, int port, int timeoutMs) {
    assert(timeoutMs > 0);
    assert(port > 0 && port <= 65535);

    ensureEnetInitialized();

    ENetAddress serverAddress = {};
    if (enet_address_set_host(&serverAddress, std::string(address).c_str()) != 0)
        throw Exception("Could not resolve address '{}'", address);
    serverAddress.port = static_cast<enet_uint16>(port);

    ENetHost *host = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);
    if (!host)
        throw Exception("Could not create a network socket");

    std::unique_ptr<NetHost> result(new NetHost(host, timeoutMs));
    ENetPeer *peer = enet_host_connect(host, &serverAddress, CHANNEL_COUNT, 0);
    if (!peer)
        throw Exception("Could not start connecting to '{}:{}'", address, port);
    setPeerTimeout(peer, timeoutMs);
    return result;
}

int NetHost::port() const {
    return _host->address.port;
}

std::optional<NetEvent> NetHost::poll(int timeoutMs) {
    assert(timeoutMs >= 0);

    ENetEvent event;
    int status = enet_host_service(_host, &event, timeoutMs);
    if (status < 0)
        throw Exception("Network error while polling for events");
    if (status == 0)
        return std::nullopt;

    NetEvent result;
    result.peer = static_cast<NetPeerId>(event.peer - _host->peers);
    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT:
        result.type = NET_EVENT_CONNECTED;
        setPeerTimeout(event.peer, _timeoutMs);
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        result.type = NET_EVENT_DISCONNECTED;
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        result.type = NET_EVENT_MESSAGE;
        result.data = Blob::copy(event.packet->data, event.packet->dataLength);
        enet_packet_destroy(event.packet);
        break;
    default:
        assert(false);
        return std::nullopt;
    }
    return result;
}

void NetHost::send(NetPeerId peer, NetChannel channel, const Blob &data) {
    assert(peer >= 0 && peer < static_cast<NetPeerId>(_host->peerCount));

    ENetPeer *enetPeer = &_host->peers[peer];
    if (enetPeer->state != ENET_PEER_STATE_CONNECTED)
        return; // The peer may have dropped since its last event, its disconnect is reported by poll.

    ENetPacket *packet = createPacket(channel, data);
    if (enet_peer_send(enetPeer, channelId(channel), packet) != 0)
        enet_packet_destroy(packet); // Enet takes ownership only on success.
}

void NetHost::broadcast(NetChannel channel, const Blob &data) {
    enet_host_broadcast(_host, channelId(channel), createPacket(channel, data));
}

void NetHost::disconnect(NetPeerId peer) {
    assert(peer >= 0 && peer < static_cast<NetPeerId>(_host->peerCount));

    enet_peer_disconnect_later(&_host->peers[peer], 0); // Plain enet_peer_disconnect drops the peer's queued packets.
}

void NetHost::flush() {
    enet_host_flush(_host);
}
