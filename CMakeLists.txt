cmake_minimum_required(VERSION 3.16)

project(mqueue LANGUAGES C)

include(GNUInstallDirs)

find_package(Check REQUIRED)

set(CMAKE_C_STANDARD 23)

set(MSGQ_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(FIBER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fiber)
set(INCLUDE_DIR include)

set(MSGQ_SRCS
  ${MSGQ_DIR}/log.h
  ${MSGQ_DIR}/shm.h
  ${MSGQ_DIR}/msgq.h
  ${MSGQ_DIR}/log.c
  ${MSGQ_DIR}/shm.c
  ${MSGQ_DIR}/msgq.c)

set(FIBER_SRCS
  ${FIBER_DIR}/fiber.h
  ${FIBER_DIR}/fiber.c)



add_executable(check_msgq test/test.c ${FIBER_SRCS}
 ${MSGQ_SRCS})

target_link_libraries(check_msgq Check::checkShared )
target_compile_options(check_msgq PRIVATE  -Wall -Wextra  -Wsign-compare)
target_include_directories(check_msgq PRIVATE ${MSGQ_DIR} ${FIBER_DIR} ${CHECK_INCLUDE_DIRS})
