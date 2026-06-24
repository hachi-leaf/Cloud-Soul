# Copyright (c) leaf
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""
cs_output.launch.py — 启动所有 cs_output 工具节点
用法: ros2 launch cs_output cs_output.launch.py agent_name:=agent
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    # ── 共享参数（同包复用） ──
    agent_name      = LaunchConfiguration('agent_name',       default='agent')
    info_period     = LaunchConfiguration('info_period',      default='1.0')   # 工具节点心跳周期
    tool_timeout    = LaunchConfiguration('tool_timeout',     default='60.0')
    info_timeout    = LaunchConfiguration('info_timeout',     default='3.0')
    discovery_period= LaunchConfiguration('discovery_period', default='1.0')
    topic_output    = LaunchConfiguration('topic_output',     default='raw_message')

    return LaunchDescription([
        DeclareLaunchArgument('agent_name',        default_value='agent'),
        DeclareLaunchArgument('info_period',       default_value='1.0'),
        DeclareLaunchArgument('tool_timeout',      default_value='60.0'),
        DeclareLaunchArgument('info_timeout',      default_value='3.0'),
        DeclareLaunchArgument('discovery_period',  default_value='1.0'),
        DeclareLaunchArgument('topic_output',      default_value='raw_message'),

        # ── output_mgmt_node（管理器） ──
        Node(
            package='cs_output',
            executable='output_mgmt_node',
            name='output_mgmt_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'tool_timeout': tool_timeout,
                'info_timeout': info_timeout,
                'discovery_period': discovery_period,
            }],
            respawn=True,
        ),

        # ── shell_exec_node ──
        Node(
            package='cs_output',
            executable='shell_exec_node',
            name='shell_exec_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_rate': info_period,
            }],
            respawn=True,
        ),

        # ── file_read_node ──
        Node(
            package='cs_output',
            executable='file_read_node',
            name='file_read_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_rate': info_period,
            }],
            respawn=True,
        ),

        # ── file_write_node ──
        Node(
            package='cs_output',
            executable='file_write_node',
            name='file_write_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_rate': info_period,
            }],
            respawn=True,
        ),

        # ── message_send_node ──
        Node(
            package='cs_output',
            executable='message_send_node',
            name='message_send_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_rate': info_period,
                'topic_output': topic_output,
            }],
            respawn=True,
        ),

        # ── web_search_node ──
        Node(
            package='cs_output',
            executable='web_search_node',
            name='web_search_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'info_period': info_period,
            }],
            respawn=True,
        ),
    ])
