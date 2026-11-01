cmake_minimum_required(VERSION 3.27 FATAL_ERROR)

set(APPLICATION_GAME_STATES_SOURCES
        CoopJoinState.cpp
        CoopMenuState.cpp
        CreditsState.cpp
        GameFsmBuilder.cpp
        LoadSlotState.cpp
        LoadStep2State.cpp
        MainMenuState.cpp
        StartState.cpp
        VideoState.cpp)

set(APPLICATION_GAME_STATES_HEADERS
        CoopJoinState.h
        CoopMenuState.h
        CreditsState.h
        GameFsmBuilder.h
        LoadSlotState.h
        LoadStep2State.h
        MainMenuState.h
        StartState.h
        VideoState.h)

add_library(application_game_states STATIC ${APPLICATION_GAME_STATES_SOURCES} ${APPLICATION_GAME_STATES_HEADERS})
target_check_style(application_game_states)

target_link_libraries(application_game_states
        PUBLIC
        engine_coop
        library_fsm
        media
        utility)
