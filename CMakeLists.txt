cmake_minimum_required(VERSION 3.20)

project(eternityII C)

set(CMAKE_C_STANDARD 99)

option(DEBUG_BUILD "Build with debug symbols" OFF)
option(NCURSES "Enable ncurses interface" OFF)

# Flag d'optimisation, calqué sur le makefile : -Ofast sous Linux (gcc) ; sous
# Darwin, clang déprécie -Ofast (-Wdeprecated-ofast) → on passe l'équivalent.
if(APPLE)
    set(OPT_FLAGS -O3 -ffast-math)
else()
    set(OPT_FLAGS -Ofast)
endif()

if(DEBUG_BUILD)
    add_compile_options(-Wall ${OPT_FLAGS} -g)
else()
    add_compile_options(-Wall ${OPT_FLAGS})
endif()

if(NCURSES)
    add_compile_definitions(USE_NCURSES)
    set(LOGGER_SRC src/ui/logger_ncurses.c)
    find_package(Curses REQUIRED)
else()
    set(LOGGER_SRC src/ui/logger.c)
endif()

add_executable(eternityII
        ${LOGGER_SRC}
        src/app/static_variables.c
        src/net/local_socket.c
        src/core/lifo.c
        src/net/tcpclient.c
        src/net/tcpserver.c
        src/core/part.c
        src/core/readdata.c
        src/core/datamanager.c
        src/core/possibility.c
        src/net/etii_protocol.c
        src/app/etii_client.c
        src/app/etii_server.c
        src/core/etii_search.c
        src/ui/command_lines.c
        src/ui/command_match.c
        src/ui/command_history.c
        src/ui/console.c
        src/app/main.c
)

# Includes explicites "domaine/x.h" résolus depuis src/.
target_include_directories(eternityII PRIVATE src)

target_link_libraries(eternityII pthread)

if(NCURSES)
    target_link_libraries(eternityII ${CURSES_LIBRARIES})
endif()