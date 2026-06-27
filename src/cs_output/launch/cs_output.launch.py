# Copyright (c) leaf
# SPDX-License-Identifier: MIT

"""
Cloud‑Soul 输出子系统 launch 文件
启动以下节点：
  - output_mgmt_node    (统一管理节点)
  - file_rdwt_node       (文件读写工具)
  - shell_exec_node      (Shell 命令执行工具)
  - message_send_node    (消息发送工具)

所有节点使用统一的 agent_name 命名空间。
通过 emulate_tty=True 确保 SIGINT 正确传递给节点，实现优雅退出。
"""

# Copyright (c) leaf
# SPDX-License-Identifier: MIT

from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration


def generate_launch_description():
    # 公共参数
    agent_name_arg = DeclareLaunchArgument('agent_name', default_value='agent')
    info_timeout_arg = DeclareLaunchArgument('info_timeout', default_value='3.0')
    discovery_period_arg = DeclareLaunchArgument('discovery_period', default_value='1.0')
    default_timeout_arg = DeclareLaunchArgument('default_timeout', default_value='60.0')
    cancel_timeout_arg = DeclareLaunchArgument('cancel_timeout', default_value='2.0')
    info_rate_arg = DeclareLaunchArgument('info_rate', default_value='1.0')
    topic_output_arg = DeclareLaunchArgument('topic_output', default_value='raw_message')

    agent_name = LaunchConfiguration('agent_name')
    info_timeout = LaunchConfiguration('info_timeout')
    discovery_period = LaunchConfiguration('discovery_period')
    default_timeout = LaunchConfiguration('default_timeout')
    cancel_timeout = LaunchConfiguration('cancel_timeout')
    info_rate = LaunchConfiguration('info_rate')
    topic_output = LaunchConfiguration('topic_output')

    # 节点定义
    output_mgmt = Node(
        package='cs_output',
        executable='output_mgmt_node',
        name='output_mgmt_node',
        parameters=[{
            'agent_name': agent_name,
            'info_timeout': info_timeout,
            'discovery_period': discovery_period,
            'default_timeout': default_timeout,
            'cancel_timeout': cancel_timeout,
        }],
        emulate_tty=True,
        output='screen',
    )

    file_rdwt = Node(
        package='cs_output',
        executable='file_rdwt_node',
        name='file_rdwt_node',
        parameters=[{
            'agent_name': agent_name,
            'info_rate': info_rate,
            'default_timeout': default_timeout,
        }],
        emulate_tty=True,
        output='screen',
    )

    shell_exec = Node(
        package='cs_output',
        executable='shell_exec_node',
        name='shell_exec_node',
        parameters=[{
            'agent_name': agent_name,
            'info_rate': info_rate,
            'default_timeout': default_timeout,
        }],
        emulate_tty=True,
        output='screen',
    )

    message_send = Node(
        package='cs_output',
        executable='message_send_node',
        name='message_send_node',
        parameters=[{
            'agent_name': agent_name,
            'info_rate': info_rate,
            'topic_output': topic_output,
            'default_timeout': default_timeout,
        }],
        emulate_tty=True,
        output='screen',
    )

    return LaunchDescription([
        agent_name_arg,
        info_timeout_arg,
        discovery_period_arg,
        default_timeout_arg,
        cancel_timeout_arg,
        info_rate_arg,
        topic_output_arg,
        output_mgmt,
        file_rdwt,
        shell_exec,
        message_send,
    ])