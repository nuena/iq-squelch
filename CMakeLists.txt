# cmake_minimum_required(VERSION <specify CMake version here>)
project(iq_squelch)

set(CMAKE_CXX_STANDARD 14)

add_executable(iq_squelch
        iq-squelch.c
        )

find_library(CZMQ czmq)
find_library(ZMQ zmq)
target_link_libraries(iq_squelch LINK_PUBLIC ${CZMQ} ${ZMQ})