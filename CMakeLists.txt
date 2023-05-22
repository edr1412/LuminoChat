cmake_minimum_required(VERSION 3.0)
project(mychat)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置编译类型，默认为Debug
# 使用 -DCMAKE_BUILD_TYPE=Release 可以覆盖
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# 启用ThreadSanitizer
option(ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

# 启用AddressSanitizer
option(ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)

if(ENABLE_ADDRESS_SANITIZER AND ENABLE_THREAD_SANITIZER)
  message(WARNING "Both AddressSanitizer and ThreadSanitizer are enabled. This may cause undefined behavior. It is recommended to use only one of them.")
endif()

if(ENABLE_ADDRESS_SANITIZER)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if(ENABLE_THREAD_SANITIZER)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fPIE -pie")
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

add_library(pubsub pubsub.cc)
target_link_libraries(pubsub hiredis)
add_executable(pubsub_test pubsub_test.cc)
set_target_properties(pubsub_test PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(pubsub_test pubsub)

add_library(cert_lib cert.cpp)
add_library(SSL_Helper_lib SSL_Helper.cpp)
target_link_libraries(SSL_Helper_lib PRIVATE cert_lib ssl crypto dl)
add_executable(sslClient ssl_client.cpp)
add_executable(sslServer ssl_server.cpp)
target_link_libraries(sslClient muduo_net muduo_base pthread SSL_Helper_lib)
target_link_libraries(sslServer muduo_net muduo_base pthread SSL_Helper_lib)

add_library(acautomaton ACAutomaton.cc)
add_executable(acautomaton_test ACAutomaton_test.cc)
set_target_properties(acautomaton_test PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(acautomaton_test acautomaton)

add_executable(chat_server server.cc)
set_target_properties(chat_server PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(chat_server protobuf_codec chat_proto pubsub hiredis_cluster)

add_executable(chat_client client.cc)
set_target_properties(chat_client PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(chat_client protobuf_codec chat_proto acautomaton)

add_executable(chat_client_test client_test.cc)
set_target_properties(chat_client_test PROPERTIES COMPILE_FLAGS "-Wno-error=shadow")
target_link_libraries(chat_client_test protobuf_codec chat_proto)

string(TOUPPER ${CMAKE_BUILD_TYPE} BUILD_TYPE)
message(STATUS "CXX_FLAGS = " ${CMAKE_CXX_FLAGS} " " ${CMAKE_CXX_FLAGS_${BUILD_TYPE}})
