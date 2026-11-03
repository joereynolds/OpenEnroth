cmake_minimum_required(VERSION 3.27 FATAL_ERROR)

set(LIBRARY_NET_SOURCES
        NetHost.cpp)

set(LIBRARY_NET_HEADERS
        NetEnums.h
        NetEvent.h
        NetHost.h)

add_library(library_net STATIC ${LIBRARY_NET_SOURCES} ${LIBRARY_NET_HEADERS})
target_check_style(library_net)
target_link_libraries(library_net
        PUBLIC
        utility
        PRIVATE
        enet)

if(OE_BUILD_TESTS)
    set(TEST_LIBRARY_NET_SOURCES
            Tests/NetHost_ut.cpp)

    add_library(test_library_net OBJECT ${TEST_LIBRARY_NET_SOURCES})
    target_link_libraries(test_library_net PUBLIC testing_unit library_net)

    target_check_style(test_library_net)

    target_link_libraries(OpenEnroth_UnitTest PUBLIC test_library_net)
endif()
