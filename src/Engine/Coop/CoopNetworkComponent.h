#pragma once

#include "Library/Platform/Proxy/ProxyEventLoop.h"

/**
 * Polls the network of `coopSession` whenever the application processes its messages. Every screen does that once per
 * frame, so connections stay alive in menus, party creation and movies too, not only in the game loop.
 */
class CoopNetworkComponent : public ProxyEventLoop {
 public:
    virtual void processMessages(PlatformEventHandler *eventHandler) override;
};
