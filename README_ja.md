# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](#) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

ROS2 ベースの AI エージェントランタイム。一つのエージェント、複数端末、クラウド同期メモリ。

---

## アーキテクチャ

```
                  ┌─────────────┐
                  │  LLM (API)  │
                  └──────┬──────┘
                         │
┌────────────┐    ┌──────┴──────┐    ┌─────────────────────────────┐
│  cs_input  │───→│   cs_core    │───→│         cs_output            │
│            │    │              │    │                              │
│ system_    │    │ agent_loop   │    │ shell_exec    file_read     │
│ status     │    │ memory_node  │    │ file_write    message_send  │
│ message_   │    │ call_openai  │    │ web_search                  │
│ receive    │    │              │    │                              │
│ input_mgmt │    │ Git リポジトリ↔│   │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

ROS2 action `/agent_loop/_action/execute_tool` で接続される 3 つのパッケージ：

| パッケージ | 役割 |
|-----------|------|
| `cs_input` | センサー：システム状態の収集、ユーザーメッセージの購読 |
| `cs_core` | エージェントループ（LLM ⇄ ツール）、Git ベースのメモリ管理 |
| `cs_output` | ツール：シェル実行、ファイル I/O、メッセージ送信、Web 検索 |

---

## 動作の仕組み

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (スナップショット) ──→ agent_loop
                   │                                         │
                   │     ┌───────────────────────────────────┘
                   │     ▼
                   │   LLM: 推論 → ツール呼び出し
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → 結果
                   │     │
                   │     ▼
                   │   memory_node: Git 日記にアーカイブ
                   │     │
                   └─────┘ (次のサイクル)
```

- ユーザーメッセージ → 即時処理
- アイドル時 → system_status ハートビートでループ
- コンテキストが閾値を超える → LLM が日記に圧縮・リセット
- メモリは Git リポジトリの Markdown として永続化。サイクル毎に pull/push

---

## クイックスタート

**前提条件**: Ubuntu 22.04, ROS2 Humble, DeepSeek API キー（または互換エンドポイント）。

```bash
# 依存パッケージ
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# ビルド
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**メモリリポジトリ** — [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul) をフォークし、SSH プッシュを設定。

**起動**:

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

ros2 run cs_core memory_node --ros-args \
  -p repo_url:="git@github.com:your/agent-soul.git" \
  -p repo_dir:="$HOME/.adam/soul_repo" &

ros2 run cs_input system_status_node &
ros2 run cs_input message_receive_node --ros-args \
  -p topic_name:="/adam/input/message_receive" &

ros2 run cs_input input_mgmt_node &
ros2 run cs_output output_mgmt_node &
ros2 run cs_output shell_exec_node &
ros2 run cs_output file_read_node &
ros2 run cs_output file_write_node &
ros2 run cs_output message_send_node &
ros2 run cs_output web_search_node &

ros2 run cs_core agent_loop_node
```

---

## メモリモデル

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # システムプロンプト（[cognitions/*.md] 参照）
│   └── COMPRESS.md    # 圧縮プロンプト
├── cognitions/
│   ├── SELF.md        # エージェントの自己認知
│   ├── MASTER.md      # ユーザープロファイル
│   ├── METHOD.md      # 行動ルール
│   └── WORLD.md       # 既知の事実
└── diaries/
    └── YYYYMMDD.md    # 日記（LLM 圧縮要約）
```

---

## ノード

### cs_core

| ノード | 説明 |
|--------|------|
| `agent_loop_node` | メインループ：スナップショット → LLM → ツール → 繰り返し |
| `memory_node` | Git メモリ：cognitions の pull、diaries の push |

### cs_input

| ノード | トピック | 説明 |
|--------|---------|------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, メモリ, ディスク, ネットワーク, ホスト名, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 String 購読 |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | センサーデータ集約 |

### cs_output

| ノード | Action | 説明 |
|--------|--------|------|
| `shell_exec_node` | execute_tool | シェルコマンド実行 |
| `file_read_node` | execute_tool | ファイル読み取り |
| `file_write_node` | execute_tool | ファイル書き込み |
| `message_send_node` | execute_tool | メール（s-nail）または ROS2 送信 |
| `web_search_node` | execute_tool | HTTP リクエスト、マルチソース切替 |
| `output_mgmt_node` | execute_tool | ツール呼び出しのルーティング |

---

## 設定

| パラメータ | ノード | デフォルト |
|-----------|------|----------|
| `agent_name` | 全ノード | `adam` |
| `repo_url` | memory_node | — 必須 |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

環境変数: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## カスタマイズ

1. [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul) をフォークし `cognitions/` を編集
2. ツール追加：`cs_output/src/` に新ノード作成、`output_mgmt_node.cpp` に登録
3. センサー追加：`cs_input/src/` に新ノード作成、`input_mgmt_node.cpp` に登録

---

## ライセンス

MIT