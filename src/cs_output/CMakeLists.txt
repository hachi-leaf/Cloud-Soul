cmake_minimum_required(VERSION 3.8)
project(cs_output)

# 默认使用 C++17
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

# 查找依赖
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)
find_package(cs_interfaces REQUIRED)

add_executable(file_read_node src/file_read.cpp)
ament_target_dependencies(file_read_node rclcpp rclcpp_action std_msgs cs_interfaces)
install(TARGETS file_read_node DESTINATION lib/${PROJECT_NAME})

add_executable(file_write_node src/file_write.cpp)
ament_target_dependencies(file_write_node rclcpp rclcpp_action std_msgs cs_interfaces)
install(TARGETS file_write_node DESTINATION lib/${PROJECT_NAME})

add_executable(shell_exec_node src/shell_exec.cpp)
ament_target_dependencies(shell_exec_node rclcpp rclcpp_action std_msgs cs_interfaces)
install(TARGETS shell_exec_node DESTINATION lib/${PROJECT_NAME})

add_executable(user_notify_node src/user_notify.cpp)
ament_target_dependencies(user_notify_node rclcpp rclcpp_action std_msgs cs_interfaces)
install(TARGETS user_notify_node DESTINATION lib/${PROJECT_NAME})

add_executable(output_mgmt_node src/output_mgmt.cpp)

# 包含路径：cs_interfaces 生成的头文件
target_include_directories(output_mgmt_node PUBLIC
  ${rclcpp_INCLUDE_DIRS}
  ${rclcpp_action_INCLUDE_DIRS}
  ${std_msgs_INCLUDE_DIRS}
  ${cs_interfaces_INCLUDE_DIRS}
)

# 链接库
ament_target_dependencies(output_mgmt_node
  rclcpp
  rclcpp_action
  std_msgs
  cs_interfaces
)

# 安装可执行文件到 lib/<package> 目录
install(TARGETS output_mgmt_node
  DESTINATION lib/${PROJECT_NAME})

# 如果需要安装 launch 或其他目录，可在此添加
# install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_package()