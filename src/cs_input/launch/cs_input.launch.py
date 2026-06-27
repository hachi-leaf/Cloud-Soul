#!/usr/bin/env python3
# Copyright (c) leaf
# SPDX-License-Identifier: MIT

"""
Cloud-Soul 输入子系统启动文件
启动 system_status_node、message_receive_node 和 input_mgmt_node，
统一 agent_name，其他参数可分别重写。
"""

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration


def generate_launch_description():
    # ---- 声明可配置参数 ----
    agent_name_arg = DeclareLaunchArgument(
        'agent_name',
        default_value='agent',
        description='Agent 命名空间，所有输入节点共用'
    )

    # system_status_node 专用参数
    sys_publish_rate_arg = DeclareLaunchArgument(
        'sys_publish_rate',
        default_value='1.0',
        description='系统状态采集频率 (Hz)'
    )

    # message_receive_node 专用参数
    msg_ros_channel_arg = DeclareLaunchArgument(
        'msg_ros_channel',
        default_value='ros2_msg',
        description='ROS 消息渠道服务名后缀'
    )
    msg_info_rate_arg = DeclareLaunchArgument(
        'msg_info_rate',
        default_value='1.0',
        description='message_receive info 发布频率 (Hz)'
    )

    # input_mgmt_node 专用参数
    mgmt_info_timeout_arg = DeclareLaunchArgument(
        'mgmt_info_timeout',
        default_value='3.0',
        description='输入源心跳超时 (秒)'
    )
    mgmt_discovery_period_arg = DeclareLaunchArgument(
        'mgmt_discovery_period',
        default_value='1.0',
        description='新输入源扫描周期 (秒)'
    )

    # ---- 节点定义 ----
    system_status_node = Node(
        package='cs_input',
        executable='system_status_node',
        name='system_status_node',
        output='screen',
        parameters=[{
            'agent_name': LaunchConfiguration('agent_name'),
            'publish_rate': LaunchConfiguration('sys_publish_rate'),
        }],
        emulate_tty=True,   # 保证信号传递
    )

    message_receive_node = Node(
        package='cs_input',
        executable='message_receive_node',
        name='message_receive_node',
        output='screen',
        parameters=[{
            'agent_name': LaunchConfiguration('agent_name'),
            'ros_channel': LaunchConfiguration('msg_ros_channel'),
            'info_rate': LaunchConfiguration('msg_info_rate'),
        }],
        emulate_tty=True,
    )

    input_mgmt_node = Node(
        package='cs_input',
        executable='input_mgmt_node',
        name='input_mgmt_node',
        output='screen',
        parameters=[{
            'agent_name': LaunchConfiguration('agent_name'),
            'info_timeout': LaunchConfiguration('mgmt_info_timeout'),
            'discovery_period': LaunchConfiguration('mgmt_discovery_period'),
        }],
        emulate_tty=True,
    )

    return LaunchDescription([
        agent_name_arg,
        sys_publish_rate_arg,
        msg_ros_channel_arg,
        msg_info_rate_arg,
        mgmt_info_timeout_arg,
        mgmt_discovery_period_arg,
        system_status_node,
        message_receive_node,
        input_mgmt_node,
    ])