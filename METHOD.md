## 经验记录 (2026-07-01)

### ROS2 Humble Fast-DDS 稳定性问题及根治方案
- **问题**: 系统运行中多个节点随机出现 libfastrtps.so 段错误，导致 action server 无响应、tool 掉线等连锁故障。影响 agent_loop_node, message_send_node, input_mgmt_node, web_fetch_node 等多个进程。
- **根因**: ROS2 Humble 默认 RMW (Fast-DDS) 稳定性不足，DDS participant 在长时间运行或负载波动时进入不一致状态
- **根治**: 切换为 CycloneDDS — `sudo apt install ros-humble-rmw-cyclonedds-cpp`，设置环境变量 `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp` 后重启所有节点
- **验证**: 切换后系统性段错误消失，action 调用稳定

### web_fetch_node: 全链路无 regex 的 HTML 解析
- std::regex 的 `.*?` 懒匹配在大 HTML 上触发回溯引擎栈溢出 (SIGSEGV signal 11, sp 触及页边界)
- 解决方案: 全部替换为手动状态机 (O(n), 无递归无回溯)
- strip_blocks: 逐字符扫描匹配 script/style/comment 起止标签
- strip_tags: 字符级跳过 <...>
- decode_html_entities: 手动扫描 & 匹配命名/数字实体
- 空白压缩: 计数器折叠连续 \n 和空格