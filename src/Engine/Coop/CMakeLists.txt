cmake_minimum_required(VERSION 3.27 FATAL_ERROR)

set(ENGINE_COOP_SOURCES
        CoopProtocol.cpp)

set(ENGINE_COOP_HEADERS
        CoopProtocol.h)

add_library(engine_coop STATIC ${ENGINE_COOP_SOURCES} ${ENGINE_COOP_HEADERS})
target_link_libraries(engine_coop PUBLIC engine library_net)
target_check_style(engine_coop)

if(OE_BUILD_TESTS)
    set(TEST_ENGINE_COOP_SOURCES
            Tests/CoopProtocol_ut.cpp)

    add_library(test_engine_coop OBJECT ${TEST_ENGINE_COOP_SOURCES})
    target_link_libraries(test_engine_coop PUBLIC testing_unit engine_coop)

    target_check_style(test_engine_coop)

    target_link_libraries(OpenEnroth_GameTest PUBLIC test_engine_coop)
endif()
