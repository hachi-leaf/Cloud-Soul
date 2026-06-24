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

## 🏗️ アーキテクチャ

```
                  ┌─────────────┐
                  │  LLM (API)  │
                  └──────┬──────┘
                         │
┌────────────┐    ┌──────┴───────┐    ┌─────────────────────────────┐
│  cs_input  │───→│   cs_core    │───→│         cs_output           │
│            │    │              │    │                             │
│ system_    │    │ agent_loop   │    │ shell_exec    file_read     │
│ status     │    │ memory_node  │    │ file_write    message_send  │
│ message_   │    │ call_openai  │    │ web_search    web_chat      │
│ receive    │    │              │    │                             │
│ input_mgmt │    │ Git repo ↔   │    │ output_mgmt                 │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

三つのパッケージ、ROS2 アクション `/agent_loop/_action/execute_tool` で接続：

| パッケージ | 役割 |
|-----------|------|
| `cs_input` | センサー：システム状態、ユーザーメッセージ購読 |
| `cs_core` | エージェントループ (LLM ⇄ ツール)、Git ベースメモリ |
| `cs_output` | ツール：シェル、ファイル I/O、メッセージ、Web 検索、Web チャット |

---

## ⚙️ 動作の仕組み

```
  system_status  ──┐
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

## 🚀 クイックスタート

**前提条件**: Ubuntu 22.04, ROS2 Humble, DeepSeek 互換の API キー。

```bash
# システム依存
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# クローン & ビルド
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**メモリリポジトリ** — [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory) をフォークし、SSH プッシュを設定。

**初回セットアップ** (対話型ウィザード):

```bash
./config
```

`~/.cloudsoul/config.yaml` を生成し、エージェント名、API 認証情報、リポジトリ URL、各種調整パラメータを設定。

**すべて起動** (1 コマンド):

```bash
./start
```

`cs_input`、`cs_output`、`cs_core`、`web_chat` (Flask SSE、ポート 8080) を自動起動。ログは `/tmp/cloudsoul_*.log` に出力。

**root で実行** (システムレベルのアクセスが必要な場合):

```bash
sudo -E ./start
```

全ノード `respawn=True` 対応 — ノードがクラッシュしても ROS2 が自動再起動。

---

## 🧠 メモリモデル

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # システムプロンプト ([cognitions/*.md] を参照)
│   └── COMPRESS.md    # 圧縮プロンプト
├── cognitions/
│   ├── SELF.md        # エージェントのアイデンティティ
│   ├── MASTER.md      # ユーザープロファイル
│   ├── METHOD.md      # 行動ルール
│   └── WORLD.md       # 既知の事実
└── diaries/
    └── YYYYMMDD.md    # 日次ログ (LLM 圧縮)
```

---

## 🔌 ノード

### cs_core

| ノード | 説明 |
|------|------|
| `agent_loop_node` | メインループ：スナップショット → LLM → ツール → 繰り返し。コンテキスト管理 |
| `memory_node` | Git リコール/アーカイブ：認知をプル、日記をプッシュ |

### cs_input

| ノード | トピック | 説明 |
|------|------|------|
| `system_status_node` | `/{agent}/input/system_status` | CPU、メモリ、ディスク、ネットワーク、ホスト名、machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 / web_chat メッセージ受信 |
| `input_mgmt_node` | `/{agent}/input` (srv) | センサーデータをスナップショットに集約 |

### cs_output

| ノード | アクション | 説明 |
|------|--------|------|
| `shell_exec_node` | execute_tool | シェルコマンド実行 |
| `file_read_node` | execute_tool | ファイル読み取り (オフセット/長さ/エンコーディング) |
| `file_write_node` | execute_tool | ファイル書き込み (上書き/追記) |
| `message_send_node` | execute_tool | メール (s-nail)、ROS2 パブリッシュ、web_chat 返信 |
| `web_search_node` | execute_tool | Web 検索、マルチエンジンフォールバック |
| `web_chat_server.py` | (Flask SSE) | ブラウザチャットインターフェース (ポート 8080) |
| `output_mgmt_node` | execute_tool | ツールノードを自動検出し呼び出しをルーティング |

---

## 🔧 設定

すべてのパラメータは `~/.cloudsoul/config.yaml`（`./config` で生成）に集約。

| パラメータ | ノード | デフォルト |
|-----------|------|---------|
| `agent_name` | 全ノード | `agent` |
| `repo.url` | memory_node | — 必須 |
| `loop.max_context_tokens` | agent_loop_node | `200000` |
| `loop.summary_turns` | agent_loop_node | `25` |
| `loop.tool_timeout` | cs_core / cs_output | `60.0` |
| `input.info_timeout` | input mgmt | `3.0` |
| `output.info_timeout` | output mgmt | `3.0` |

LLM 設定は `config.yaml` の `llms` リスト内にあり、`default_llm` で使用する LLM を指定。

---

## 🎨 カスタマイズ

1. [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory) をフォークし `cognitions/` を編集
2. ツール追加：`cs_output/src/` にノードを作成し `/{agent}/output/<name>/info` トピック（JSONツール記述子、`std_msgs/String`、QoS transient_local）を発行。`output_mgmt_node` が自動検出
3. センサー追加：`cs_input/src/` にノードを作成し `/{agent}/input/<name>/info` トピック（`InputInfo` メッセージ、QoS transient_local）と `/{agent}/input/<name>` データを発行。`input_mgmt_node` が自動検出
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — 全タイムアウト、エラーコード、メッセージ文字列を一元管理

---

## 📄 ライセンス

MIT