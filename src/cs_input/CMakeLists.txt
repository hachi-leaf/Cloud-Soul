cmake_minimum_required(VERSION 3.8)
project(cs_input)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(cs_interfaces REQUIRED)

# ---------- input_mgmt_node ----------
add_executable(input_mgmt_node src/input_mgmt_node.cpp)
target_include_directories(input_mgmt_node PUBLIC
  ${rclcpp_INCLUDE_DIRS}
  ${std_msgs_INCLUDE_DIRS}
  ${cs_interfaces_INCLUDE_DIRS}
)
ament_target_dependencies(input_mgmt_node
  rclcpp
  std_msgs
  cs_interfaces
)
install(TARGETS input_mgmt_node
  DESTINATION lib/${PROJECT_NAME}
)

# ---------- system_status_node ----------
add_executable(system_status_node src/system_status_node.cpp)
ament_target_dependencies(system_status_node
  rclcpp
  std_msgs
  cs_interfaces
)
install(TARGETS system_status_node
  DESTINATION lib/${PROJECT_NAME}
)

# ---------- message_receive_node ----------
add_executable(message_receive_node src/message_receive_node.cpp)
ament_target_dependencies(message_receive_node
  rclcpp
  std_msgs
  cs_interfaces
)
install(TARGETS message_receive_node
  DESTINATION lib/${PROJECT_NAME}
)

# ---------- launch ----------
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_package()