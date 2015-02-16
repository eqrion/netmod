cmake_minimum_required(VERSION 3.0)
project(netmod)

option(USE_IPV6 "Configure the library to use the IPV6 protocol." ON)

list(APPEND NETMOD_INCLUDES
				"include/bit_stream.h"
				"include/circular_allocator.h"
				"include/network.h"
				"include/network_session.h"
				"include/uuid.h"
				)
list(APPEND NETMOD_SRCS
				"source/network_session.cpp"
				)

source_group("include\\" FILES ${NETMOD_INCLUDES})
source_group("source\\" FILES ${NETMOD_SRCS})
add_library(netmod ${NETMOD_SRCS} ${NETMOD_INCLUDES})
target_include_directories(netmod PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (USE_IPV6)

	target_compile_definitions(netmod PUBLIC NETWORK_USE_IPV6)

endif(USE_IPV6)

add_executable(chat_server "source/chat_server.cpp")
target_link_libraries(chat_server PUBLIC netmod)

add_executable(chat_client "source/chat_client.cpp")
target_link_libraries(chat_client PUBLIC netmod)