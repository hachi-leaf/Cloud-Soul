# RDK S600 / S100 Camera Expansion Board — 资料汇总
采集时间: 2026-06-30
数据源: D-Robotics GitHub, developer.d-robotics.cc

## RDK S600 规格
CPU: 18×A78AE 2.0GHz | BPU: 4×Nash 560TOPS | RAM: 32/64GB LPDDR5 | 存储: 64/256GB UFS 3.1 + NVMe
网络: 2×10GbE + 2×1GbE + 1×1GbE(MCU) | USB: 6×3.2Gen1 + 1×2.0Type-C
扩展: 2×Camera Expansion, 1×MCU Port Expansion | 尺寸: 140×123×78mm
BSP: V5.1.0, Ubuntu 24.04 noble, 6.1.158-rt58, arm-gnu-13.2

## S600 Camera扩展板
解串器: 2×MAX96712 | GMSL: 2×FAKRA-Mini 4in1 → 8路GMSL
电源: 12V DC, max 4.8A | 温度: 0~65°C
连接器: DC-044B-D025, DY11-080SB-1, 112038-161410

## S600 MCU扩展板
5×CAN FD(8Mbps), 1×30pin(7ADC/2I2C/2SPI), BMI088 IMU, 70×70×17mm

## S600 LLM工具链 1.0.2
LLM: Qwen3-8B/4B/1.7B/0.6B, DeepSeek-R1-Distill-Qwen-1.5B
VLM: Qwen3-VL-8B/4B/2B, Qwen2.5-VL-7B/3B, InternVL2-2B
VLA: Pi0 | ASR: whisper-medium
SDK: D-Robotics_LLM_S600_1.0.2_SDK.tar.gz

## RDK S100 规格
CPU: 6×A78AE | BPU: Nash 80/128TOPS | GPU: Mali-G78AE | RAM: 12/24GB LPDDR5
存储: 64GB EMMC + NVMe | 网络: 2×1GbE | 电源: 12-20V max 150W
S100(KS1E55Y): 1.5GHz/12GB/80TOPS | S100P(KS1P75Y): 2.0GHz/24GB/128TOPS

## S100 Camera扩展板
解串器: 1×MAX96712 | MIPI: 2×22pin CSI-2 | GMSL: 1×FAKRA-Mini 4in1 → 4路
电源: 12V DC, max 2.4A (超700mA需外部供电) | 温度: 0~45°C
兼容相机: IMX219, SC230AI, SC132GS, AR0820C, LEC28736A11, D457, D435i

## S100 MCU扩展板
5×CAN FD, 1×RJ45(MCU), BMI088 IMU, 0~45°C

## 12L
未在任何 D-Robotics 公开文档中找到，搜索持续中。

## 文档链接
S600: developer.d-robotics.cc/rdk_s_doc/01_Quick_start/01_hardware_introduction/02_rdk_s600/01_rdk_s600_kit
S100: developer.d-robotics.cc/rdk_s_doc/01_Quick_start/01_hardware_introduction/01_rdk_s100/01_rdk_s100_kit
GitHub: github.com/D-Robotics/rdk_s_doc

## VLA (Pi0) 案例
- 硬件: RDK S600
- 模型: Pi0, 端侧推理 98ms total
  - pre-process: 1.5ms, vision: 18.5ms, language: 39ms, action: 38.5ms, post-process: 0.05ms
  - 内存: 4.4GB
- 仿真: RoboTwin (github.com/D-Robotics/RoboTwin, branch pi0pub)
- 支持自训练模型 + 实机联调

## 配件: GS130W 双目相机
- 传感器: 双SC132GS 全局快门
- 基线: 80mm
- 分辨率: 1280×1080, 最高120fps
- HDR, SNR 40dB, 850/940nm NIR
- 兼容: X5, S100(需Camera板), S600
- 接口: 双MIPI

## 配件: GS130Wi 双目相机
- 同GS130W + 红外结构光

## S600 VIN 相机传感器列表
(来源: /app/multimedia_samples/sample_vin/get_vin_data -h)
| 索引 | 传感器 | 配置 |
|------|--------|------|
| 0 | imx219-30fps | 1920x1080 raw10 30fps 1lane |
| 1 | sc1336_gmsl-30fps | 1280x720 raw10 30fps 2lane (GMSL) |
| 2 | ar0820std-30fps | 3840x2160 30fps 1lane (GMSL) |
| 3 | ar0820std-1080p30 | 1920x1080 yuv 30fps 1lane (GMSL) |
| 4 | ovx3cstd-30fps | 1920x1280 yuv 30fps 1lane (GMSL Serdes) |

GMSL 参数: -l link_port (0:A 1:B 2:C 3:D), -m mipi_rx (MIPI host 1/2)

## LeRobot + BPU 部署
- rdk_LeRobot_tools: https://github.com/D-Robotics/rdk_LeRobot_tools
- SO100 单臂/双臂性能对比
- 全流程: 校准→遥操→数据集→训练→CPU推理→ONNX导出→BPU编译→BPU部署
- 社区博客: https://developer.d-robotics.cc/forumDetail/289424806557116771
- Feishu: https://horizonrobotics.feishu.cn/docx/HSr8dBdZ0oQ5OwxPQvBcsuyZnWe

## S600 V5.1.0 Release (2026-06-12)
- NVMe 启动支持（Flash分区变化, 必须整烧）
- ACore CAN (C+Python)
- ACore UART/SPI
- 新相机驱动: shw3h, shw3f, MC30E0Z23-00-03
- ORBBEC 335LG 深度相机
- 10GbE 需固件下载才有万兆性能
- NVMe 启动不支持休眠唤醒
- V1P0 硬件版本

## 社区项目
- ros2-patrol-robot (wangshichen0506): ROS2停车巡检机器人, 适配S100
- HeyRobotModelApis (sunshineharry): S600端侧OpenAI兼容API (Qwen3-VL-8B + whisper-medium)

## 硬件资料下载
### S600 Camera Extension Board
- 3D Model: https://archive.d-robotics.cc/downloads/hardware/rdk_s600/rdk_s600_camera_expansion_board/RDK_S600_CAMERA_EXPANSION_BOARD_3D_Model_V1P0_20260420.STEP
- Brief (PDF): https://archive.d-robotics.cc/downloads/hardware/rdk_s600/rdk_s600_camera_expansion_board/drobotics_rdk_s600_camera_expansion_board_brief_v1p0_zh.pdf
- Hardware Reference Design: https://archive.d-robotics.cc/downloads/hardware/rdk_s600/rdk_s600_camera_expansion_board/hardware_reference_design/

### S100 Camera Extension Board
- Manual (PDF): https://archive.d-robotics.cc/downloads/hardware/rdk_s100/rdk_s100_camera_expansion_board/d_robotics_rdk_s100_camera_expansion_board_zh_v1_1.pdf
- Pinlist (XLSX): https://archive.d-robotics.cc/downloads/hardware/rdk_s100/rdk_s100_camera_expansion_board/drobotics_rdk_s100_camera_expansion_board_pinlist_v1p0_0924.xlsx
- Reference Design (ZIP): https://archive.d-robotics.cc/downloads/hardware/rdk_s100/rdk_s100_camera_expansion_board/drobotics_rdk_s100_camera_expansion_board_reference_design_v1p0.zip
- 3D Model (STEP): https://archive.d-robotics.cc/downloads/hardware/rdk_s100/rdk_s100_camera_expansion_board/drobotics_rdk_s100_camera_expansion_board_v1p0.step
