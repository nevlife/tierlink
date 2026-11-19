cmake_minimum_required(VERSION 3.16)
project(tierlink LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

find_package(ament_cmake REQUIRED)
find_package(zenoh_cpp_vendor REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(${PROJECT_NAME} SHARED
  src/aimd.cpp
  src/control.cpp
  src/delay_tracker.cpp
  src/ledbat.cpp
  src/link_manager.cpp
  src/outbox.cpp
  src/params.cpp
  src/scheduler.cpp
  src/teleop_messages.cpp
  src/trendline.cpp
  src/wire.cpp
  src/zenoh_link.cpp
)
target_include_directories(${PROJECT_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include/${PROJECT_NAME}>
)
target_link_libraries(${PROJECT_NAME} PUBLIC zenohcxx::zenohc PRIVATE yaml-cpp::yaml-cpp)

add_executable(tierlink_server src/server_main.cpp)
target_link_libraries(tierlink_server ${PROJECT_NAME})
add_executable(tierlink_edge src/edge_main.cpp)
target_link_libraries(tierlink_edge ${PROJECT_NAME})
add_executable(tierlink_bench tools/bench/bench_main.cpp)
target_link_libraries(tierlink_bench ${PROJECT_NAME})
# A debugging operator: arms the vehicle and drives at a fixed speed, without video.
add_executable(tierlink_teleop tools/teleop/teleop_main.cpp)
target_link_libraries(tierlink_teleop ${PROJECT_NAME})

install(DIRECTORY include/ DESTINATION include/${PROJECT_NAME})
install(TARGETS tierlink_server tierlink_edge tierlink_bench tierlink_teleop DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})
install(TARGETS ${PROJECT_NAME} EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  foreach(name rate_controller delay_tracker trendline link_manager outbox scheduler wire zenoh_link control teleop_messages)
    ament_add_gtest(test_${name} test/test_${name}.cpp)
    target_link_libraries(test_${name} ${PROJECT_NAME})
  endforeach()
endif()

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(zenoh_cpp_vendor yaml-cpp)
ament_package()
