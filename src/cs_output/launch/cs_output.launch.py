# Copyright (c) leaf
# SPDX-License-Identifier: MIT

"""
Cloud‑Soul 输出子系统 launch 文件

启动节点：output_mgmt_node / file_rdwt_node / shell_exec_node / message_send_node

参数映射:
                    output_mgmt  file_rdwt  shell_exec  message_send
agent_name              ✓           ✓          ✓            ✓
default_timeout (30s)   ✓           ✓          ✓            ✓
info_rate (1Hz)                     ✓          ✓            ✓
info_timeout (3s)       ✓
discovery_period (1s)   ✓
cancel_timeout (2s)     ✓
delay_timeout (5s)      ✓
topic_output                                        ✓

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
    default_timeout_arg = DeclareLaunchArgument('default_timeout', default_value='30.0')
    cancel_timeout_arg = DeclareLaunchArgument('cancel_timeout', default_value='2.0')
    delay_timeout_arg = DeclareLaunchArgument('delay_timeout', default_value='5.0')
    info_rate_arg = DeclareLaunchArgument('info_rate', default_value='1.0')
    topic_output_arg = DeclareLaunchArgument('topic_output', default_value='raw_message')
    repo_dir_arg = DeclareLaunchArgument('repo_dir', default_value='')
    max_results_arg = DeclareLaunchArgument('max_results', default_value='10')
    max_size_mb_arg = DeclareLaunchArgument('max_size_mb', default_value='5')

    agent_name = LaunchConfiguration('agent_name')
    info_timeout = LaunchConfiguration('info_timeout')
    discovery_period = LaunchConfiguration('discovery_period')
    default_timeout = LaunchConfiguration('default_timeout')
    cancel_timeout = LaunchConfiguration('cancel_timeout')
    delay_timeout = LaunchConfiguration('delay_timeout')
    info_rate = LaunchConfiguration('info_rate')
    topic_output = LaunchConfiguration('topic_output')
    repo_dir = LaunchConfiguration('repo_dir')
    max_results = LaunchConfiguration('max_results')
    max_size_mb = LaunchConfiguration('max_size_mb')

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
            'delay_timeout': delay_timeout,
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

    web_search = Node(
        package='cs_output',
        executable='web_search_node',
        name='web_search_node',
        parameters=[{
            'agent_name': agent_name,
            'info_rate': info_rate,
            'default_timeout': default_timeout,
            'max_results': max_results,
        }],
        emulate_tty=True,
        output='screen',
    )

    skills_loader = Node(
        package='cs_output',
        executable='skills_loader_node',
        name='skills_loader_node',
        parameters=[{
            'agent_name': agent_name,
            'repo_dir': repo_dir,
            'info_rate': info_rate,
            'default_timeout': default_timeout,
        }],
        emulate_tty=True,
        output='screen',
    )

    web_fetch = Node(
        package='cs_output',
        executable='web_fetch_node',
        name='web_fetch_node',
        parameters=[{
            'agent_name': agent_name,
            'info_rate': info_rate,
            'default_timeout': default_timeout,
            'max_size_mb': max_size_mb,
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
        delay_timeout_arg,
        info_rate_arg,
        topic_output_arg,
        repo_dir_arg,
        max_results_arg,
        max_size_mb_arg,
        output_mgmt,
        file_rdwt,
        shell_exec,
        message_send,
        skills_loader,
        web_search,
        web_fetch,
    ])