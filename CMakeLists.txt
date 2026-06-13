cmake_minimum_required(VERSION 3.20)

project(eternityII C)

set(CMAKE_C_STANDARD 99)

option(DEBUG_BUILD "Build with debug symbols" OFF)
option(NCURSES "Enable ncurses interface" OFF)

if(DEBUG_BUILD)
    add_compile_options(-Wall -Ofast -g)
else()
    add_compile_options(-Wall -Ofast)
endif()

if(NCURSES)
    add_compile_definitions(USE_NCURSES)
    set(LOGGER_SRC logger_ncurses.c)
    find_package(Curses REQUIRED)
else()
    set(LOGGER_SRC logger.c)
endif()

add_executable(eternityII
        ${LOGGER_SRC}
        static_variables.c
        local_socket.c
        lifo.c
        tcpclient.c
        tcpserver.c
        part.c
        readdata.c
        datamanager.c
        possibility.c
        etii_protocol.c
        etii_client.c
        etii_server.c
        etii_search.c
        command_lines.c
        command_history.c
        console.c
        main.c
)

target_link_libraries(eternityII pthread)

if(NCURSES)
    target_link_libraries(eternityII ${CURSES_LIBRARIES})
endif()