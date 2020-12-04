cmake_minimum_required(VERSION 3.10)
project(iq_squelch)

set(CMAKE_C_STANDARD 11)

add_executable(iq_squelch
        iq-squelch.c
        )

find_library(CZMQ czmq)
find_library(ZMQ zmq)
find_library(GSL gsl)
target_link_libraries(iq_squelch LINK_PUBLIC ${CZMQ} ${ZMQ} ${GSL})