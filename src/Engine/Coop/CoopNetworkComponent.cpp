#include "CoopNetworkComponent.h"

#include "CoopSession.h"

void CoopNetworkComponent::processMessages(PlatformEventHandler *eventHandler) {
    ProxyEventLoop::processMessages(eventHandler);
    if (coopSession)
        coopSession->pollNetwork();
}
