#!/usr/bin/env python3
# Copyright (c) leaf
# SPDX-License-Identifier: MIT

"""
Cloud-Soul 输入子系统启动文件

启动节点：system_status_node / message_receive_node / input_mgmt_node

参数映射:
                    sys_status  msg_receive  input_mgmt
agent_name              ✓           ✓            ✓
publish_rate (1Hz)      ✓
info_rate (1Hz)                     ✓
ros_channel ("ros2_msg")           ✓
web_chat_channel ("web_chat")      ✓
info_timeout (3s)                              ✓
discovery_period (1s)                           ✓
"""

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration


def generate_launch_description():
    # 公共参数
    agent_name_arg = DeclareLaunchArgument('agent_name', default_value='agent')
    publish_rate_arg = DeclareLaunchArgument('publish_rate', default_value='1.0')
    info_rate_arg = DeclareLaunchArgument('info_rate', default_value='1.0')
    ros_channel_arg = DeclareLaunchArgument('ros_channel', default_value='ros2_msg')
    web_chat_channel_arg = DeclareLaunchArgument('web_chat_channel', default_value='web_chat')
    info_timeout_arg = DeclareLaunchArgument('info_timeout', default_value='3.0')
    discovery_period_arg = DeclareLaunchArgument('discovery_period', default_value='1.0')

    agent_name = LaunchConfiguration('agent_name')
    publish_rate = LaunchConfiguration('publish_rate')
    info_rate = LaunchConfiguration('info_rate')
    ros_channel = LaunchConfiguration('ros_channel')
    web_chat_channel = LaunchConfiguration('web_chat_channel')
    info_timeout = LaunchConfiguration('info_timeout')
    discovery_period = LaunchConfiguration('discovery_period')

    # 节点定义
    system_status_node = Node(
        package='cs_input',
        executable='system_status_node',
        name='system_status_node',
        output='screen',
        parameters=[{
            'agent_name': agent_name,
            'publish_rate': publish_rate,
        }],
        emulate_tty=True,
    )

    message_receive_node = Node(
        package='cs_input',
        executable='message_receive_node',
        name='message_receive_node',
        output='screen',
        parameters=[{
            'agent_name': agent_name,
            'ros_channel': ros_channel,
            'web_chat_channel': web_chat_channel,
            'info_rate': info_rate,
        }],
        emulate_tty=True,
    )

    input_mgmt_node = Node(
        package='cs_input',
        executable='input_mgmt_node',
        name='input_mgmt_node',
        output='screen',
        parameters=[{
            'agent_name': agent_name,
            'info_timeout': info_timeout,
            'discovery_period': discovery_period,
        }],
        emulate_tty=True,
    )

    return LaunchDescription([
        agent_name_arg,
        publish_rate_arg,
        info_rate_arg,
        ros_channel_arg,
        web_chat_channel_arg,
        info_timeout_arg,
        discovery_period_arg,
        system_status_node,
        message_receive_node,
        input_mgmt_node,
    ])