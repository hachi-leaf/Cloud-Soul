# Copyright (c) leaf
# SPDX-License-Identifier: MIT

#!/usr/bin/env python3
"""
Cloud-Soul 核心子系统启动文件

启动节点：memory_node / agent_loop_node

参数映射:
                    memory_node  agent_loop
agent_name              ✓            ✓
repo_url                ✓
repo_dir                ✓
repo_name               ✓
repo_fork               ✓
rule_path               ✓
pull_retry_max (3)      ✓
push_retry_max (5)      ✓
context_dir                         ✓
max_context_tokens                  ✓
summary_turns                       ✓
openai_base_url          ✓            ✓
openai_api_key           ✓            ✓
openai_model             ✓            ✓
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    # ── 共享参数 ──
    agent_name         = LaunchConfiguration('agent_name',          default='agent')
    repo_url           = LaunchConfiguration('repo_url',            default='')
    repo_name          = LaunchConfiguration('repo_name',           default='origin')
    repo_fork          = LaunchConfiguration('repo_fork',           default='main')
    repo_dir           = LaunchConfiguration('repo_dir',            default='')
    rule_path          = LaunchConfiguration('rule_path',           default='prompts/RULE.md')
    pull_retry_max     = LaunchConfiguration('pull_retry_max',      default='3')
    push_retry_max     = LaunchConfiguration('push_retry_max',      default='5')
    context_dir        = LaunchConfiguration('context_dir',         default='~/.cloudsoul/contexts')
    max_context_tokens = LaunchConfiguration('max_context_tokens',  default='200000')
    summary_turns      = LaunchConfiguration('summary_turns',       default='30')
    openai_base_url    = LaunchConfiguration('openai_base_url',     default='https://api.deepseek.com')
    openai_api_key     = LaunchConfiguration('openai_api_key',      default='')
    openai_model       = LaunchConfiguration('openai_model',        default='deepseek-v4-pro')

    return LaunchDescription([
        DeclareLaunchArgument('agent_name',          default_value='agent'),
        DeclareLaunchArgument('repo_url',            default_value=''),
        DeclareLaunchArgument('repo_name',           default_value='origin'),
        DeclareLaunchArgument('repo_fork',           default_value='main'),
        DeclareLaunchArgument('repo_dir',            default_value=''),
        DeclareLaunchArgument('rule_path',           default_value='prompts/RULE.md'),
        DeclareLaunchArgument('pull_retry_max',      default_value='3'),
        DeclareLaunchArgument('push_retry_max',      default_value='5'),
        DeclareLaunchArgument('context_dir',         default_value='~/.cloudsoul/contexts'),
        DeclareLaunchArgument('max_context_tokens',  default_value='200000'),
        DeclareLaunchArgument('summary_turns',       default_value='30'),
        DeclareLaunchArgument('openai_base_url',     default_value='https://api.deepseek.com'),
        DeclareLaunchArgument('openai_api_key',      default_value=''),
        DeclareLaunchArgument('openai_model',        default_value='deepseek-v4-pro'),

        # ── memory_node ──
        Node(
            package='cs_core',
            executable='memory_node',
            name='memory_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'repo_url': repo_url,
                'repo_name': repo_name,
                'repo_fork': repo_fork,
                'repo_dir': repo_dir,
                'rule_path': rule_path,
                'pull_retry_max': pull_retry_max,
                'push_retry_max': push_retry_max,
                'openai_base_url': openai_base_url,
                'openai_api_key': openai_api_key,
                'openai_model': openai_model,
            }],
            respawn=True,
        ),

        # ── agent_loop_node ──
        Node(
            package='cs_core',
            executable='agent_loop_node',
            name='agent_loop_node',
            namespace=agent_name,
            output='both',
            parameters=[{
                'agent_name': agent_name,
                'context_dir': context_dir,
                'max_context_tokens': max_context_tokens,
                'summary_turns': summary_turns,
                'openai_base_url': openai_base_url,
                'openai_api_key': openai_api_key,
                'openai_model': openai_model,
            }],
            respawn=True,
        ),
    ])