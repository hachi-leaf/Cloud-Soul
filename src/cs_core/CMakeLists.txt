cmake_minimum_required(VERSION 3.8)
project(cs_core)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(cs_interfaces REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED libgit2)

find_package(CURL REQUIRED)
find_package(std_msgs REQUIRED)

# ---------- mrymt_node ----------
add_executable(mrymt_node
  src/mrymt_node.cpp
  src/call_openai.cpp
)

target_include_directories(mrymt_node PRIVATE
  include
  ${LIBGIT2_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(mrymt_node
  rclcpp::rclcpp
  ${LIBGIT2_LIBRARIES}
  ${CURL_LIBRARIES}
  nlohmann_json::nlohmann_json
)

ament_target_dependencies(mrymt_node cs_interfaces)

install(TARGETS mrymt_node
  DESTINATION lib/${PROJECT_NAME}
)

# ---------- agent_loop_node ----------
add_executable(agent_loop_node
  src/agent_loop_node.cpp
  src/call_openai.cpp
)

target_include_directories(agent_loop_node PRIVATE
  include
  ${CURL_INCLUDE_DIRS}
)

target_link_libraries(agent_loop_node
  rclcpp::rclcpp
  rclcpp_action::rclcpp_action
  ${CURL_LIBRARIES}
  nlohmann_json::nlohmann_json
)

ament_target_dependencies(agent_loop_node cs_interfaces std_msgs)

install(TARGETS agent_loop_node
  DESTINATION lib/${PROJECT_NAME}
)

# ---------- launch ----------
install(DIRECTORY launch
  DESTINATION share/${PROJECT_NAME}
)

ament_package()
