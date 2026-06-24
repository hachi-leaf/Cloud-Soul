cmake_minimum_required(VERSION 3.8)
project(cs_output)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

# 查找依赖
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)
find_package(cs_interfaces REQUIRED)
find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(nlohmann_json REQUIRED)

# 定义可执行文件及其源文件
set(EXECUTABLE_LIST
  output_mgmt_node    src/output_mgmt_node.cpp
  shell_exec_node     src/shell_exec_node.cpp
  file_read_node      src/file_read_node.cpp
  file_write_node     src/file_write_node.cpp
  message_send_node   src/message_send_node.cpp
  web_search_node     src/web_search_node.cpp
)

# 批量创建可执行文件
list(LENGTH EXECUTABLE_LIST EXEC_COUNT)
set(INDEX 0)
while(INDEX LESS EXEC_COUNT)
  list(GET EXECUTABLE_LIST ${INDEX} TARGET_NAME)
  math(EXPR INDEX "${INDEX} + 1")
  list(GET EXECUTABLE_LIST ${INDEX} SOURCE_FILE)
  math(EXPR INDEX "${INDEX} + 1")

  add_executable(${TARGET_NAME} ${SOURCE_FILE})
  ament_target_dependencies(${TARGET_NAME}
    rclcpp
    rclcpp_action
    std_msgs
    cs_interfaces
  )

  # web_search_node 需要额外链接 libcurl、libxml2、nlohmann_json
  if(TARGET_NAME STREQUAL "web_search_node")
    target_link_libraries(${TARGET_NAME}
      ${CURL_LIBRARIES}
      ${LIBXML2_LIBRARIES}
      nlohmann_json::nlohmann_json
    )
    target_include_directories(${TARGET_NAME} PRIVATE
      ${CURL_INCLUDE_DIRS}
      ${LIBXML2_INCLUDE_DIR}
    )
  endif()

  # Add cs_interfaces headers for constants.hpp
  target_include_directories(${TARGET_NAME} PRIVATE ${cs_interfaces_INCLUDE_DIRS})
  install(TARGETS ${TARGET_NAME}
    DESTINATION lib/${PROJECT_NAME})
endwhile()

# ---------- launch ----------
install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_package()