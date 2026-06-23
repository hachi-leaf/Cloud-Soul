#!/usr/bin/env python3
"""
cs_core.launch.py — 启动 memory_node 和 agent_loop_node
用法: ros2 launch cs_core cs_core.launch.py agent_name:=agent_test \
        repo_url:=https://github.com/hachi-leaf/Adam-Soul \
        repo_dir:=/home/leaf-jammy/.cloudsoul/soul \
        openai_base_url:=https://api.deepseek.com \
        openai_api_key:=sk-xxx
"""

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    # ── 共享参数 ──
    agent_name        = LaunchConfiguration('agent_name',         default='agent_test')
    repo_url          = LaunchConfiguration('repo_url',           default='')
    repo_name         = LaunchConfiguration('repo_name',          default='origin')
    repo_fork         = LaunchConfiguration('repo_fork',          default='main')
    repo_dir          = LaunchConfiguration('repo_dir',           default='')
    rule_path         = LaunchConfiguration('rule_path',          default='prompts/RULE.md')
    context_dir       = LaunchConfiguration('context_dir',        default='~/.cloudsoul/contexts')
    max_context_tokens= LaunchConfiguration('max_context_tokens', default='200000')
    summary_turns     = LaunchConfiguration('summary_turns',      default='30')
    loop_rate         = LaunchConfiguration('loop_rate',          default='0.0')
    tool_timeout      = LaunchConfiguration('tool_timeout',       default='60.0')
    openai_base_url   = LaunchConfiguration('openai_base_url',    default='https://api.deepseek.com')
    openai_api_key    = LaunchConfiguration('openai_api_key',     default='')
    openai_model      = LaunchConfiguration('openai_model',       default='deepseek-v4-pro')

    return LaunchDescription([
        DeclareLaunchArgument('agent_name',          default_value='agent_test'),
        DeclareLaunchArgument('repo_url',            default_value=''),
        DeclareLaunchArgument('repo_name',           default_value='origin'),
        DeclareLaunchArgument('repo_fork',           default_value='main'),
        DeclareLaunchArgument('repo_dir',            default_value=''),
        DeclareLaunchArgument('rule_path',           default_value='prompts/RULE.md'),
        DeclareLaunchArgument('context_dir',         default_value='~/.cloudsoul/contexts'),
        DeclareLaunchArgument('max_context_tokens',  default_value='200000'),
        DeclareLaunchArgument('summary_turns',       default_value='30'),
        DeclareLaunchArgument('loop_rate',           default_value='0.0'),
        DeclareLaunchArgument('tool_timeout',        default_value='60.0'),
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
                'loop_rate': loop_rate,
                'tool_timeout': tool_timeout,
                'openai_base_url': openai_base_url,
                'openai_api_key': openai_api_key,
                'openai_model': openai_model,
            }],
            respawn=True,
        ),
    ])