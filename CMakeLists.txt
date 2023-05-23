cmake_minimum_required(VERSION 3.0)
project(mychat)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
include_directories(/usr/local/include)
link_directories(/usr/local/lib)
add_definitions(-DMUDUO_STD_STRING)
set(CMAKE_CXX_STANDARD 17)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(protobuf_codec codec.cc)
target_link_libraries(protobuf_codec protobuf muduo_net muduo_base pthread z)

add_custom_command(OUTPUT chat.pb.cc chat.pb.h
  COMMAND protoc
  ARGS --cpp_out . ${CMAKE_CURRENT_SOURCE_DIR}/chat.proto -I${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS chat.proto
  VERBATIM )

set_source_files_properties(chat.pb.cc PROPERTIES COMPILE_FLAGS "-Wno-conversion -Wno-shadow")
include_directories(${PROJECT_BINARY_DIR})

add_library(chat_proto chat.pb.cc)
target_link_libraries(chat_proto protobuf pthread)

add_library(acautomaton ACAutomaton.cc)

add_executable(chat_client client.cc)
set_target_properties(chat_client PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(chat_client protobuf_codec chat_proto acautomaton)

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})
