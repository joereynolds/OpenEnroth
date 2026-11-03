cmake_minimum_required(VERSION 3.27 FATAL_ERROR)

set(ENGINE_COOP_SOURCES
        CoopClient.cpp
        CoopGuestSave.cpp
        CoopNetworkComponent.cpp
        CoopProtocol.cpp
        CoopServer.cpp
        CoopSession.cpp)

set(ENGINE_COOP_HEADERS
        CoopClient.h
        CoopEnums.h
        CoopGuestSave.h
        CoopNetworkComponent.h
        CoopProtocol.h
        CoopServer.h
        CoopSession.h)

add_library(engine_coop STATIC ${ENGINE_COOP_SOURCES} ${ENGINE_COOP_HEADERS})
target_link_libraries(engine_coop PUBLIC engine library_net library_platform_proxy)
target_check_style(engine_coop)

if(OE_BUILD_TESTS)
    set(TEST_ENGINE_COOP_SOURCES
            Tests/CoopProtocol_ut.cpp
            Tests/CoopSession_ut.cpp)

    add_library(test_engine_coop OBJECT ${TEST_ENGINE_COOP_SOURCES})
    target_link_libraries(test_engine_coop PUBLIC testing_unit engine_coop)

    target_check_style(test_engine_coop)

    target_link_libraries(OpenEnroth_GameTest PUBLIC test_engine_coop)
endif()
