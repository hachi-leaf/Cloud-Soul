# Copyright (c) leaf
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""
cs_input.launch.py — 启动所有 cs_input 节点
用法: ros2 launch cs_input cs_input.launch.py agent_name:=agent
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    # ── 共享参数 ──
    agent_name      = LaunchConfiguration('agent_name',       default='agent')
    info_timeout    = LaunchConfiguration('info_timeout',     default='3.0')
    discovery_period= LaunchConfiguration('discovery_period', default='1.0')
    info_rate       = LaunchConfiguration('info_rate',        default='1.0')
    publish_rate    = LaunchConfiguration('publish_rate',     default='1.0')
    ros_channel     = LaunchConfiguration('ros_channel',      default='ros2_msg')

    return LaunchDescription([
        DeclareLaunchArgument('agent_name',        default_value='agent'),
        DeclareLaunchArgument('info_timeout',      default_value='3.0'),
        DeclareLaunchArgument('discovery_period',  default_value='1.0'),
        DeclareLaunchArgument('info_rate',         default_value='1.0'),
        DeclareLaunchArgument('publish_rate',      default_value='1.0'),
        DeclareLaunchArgument('ros_channel',       default_value='ros2_msg'),

        # ── input_mgmt_node ──
        Node(
            package='cs_input',
            executable='input_mgmt_node',
            name='input_mgmt_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_timeout': info_timeout,
                'discovery_period': discovery_period,
            }],
            respawn=True,
        ),

        # ── system_status_node ──
        Node(
            package='cs_input',
            executable='system_status_node',
            name='system_status_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'publish_rate': publish_rate,
            }],
            respawn=True,
        ),

        # ── message_receive_node ──
        Node(
            package='cs_input',
            executable='message_receive_node',
            name='message_receive_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_rate': info_rate,
                'ros_channel': ros_channel,
            }],
            respawn=True,
        ),
    ])