<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">


# Cloud-Soul

### *ROS2 ネイティブ AI エージェント ランタイム — 一つの魂、どこにでも*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 Cloud-Soul とは？

> **Cloud-Soul** は ROS2 上に構築された AI エージェント OS です。
> 複数のマシンにまたがって生きる**デジタル生命体**と考えてください。
> 単一の記憶が Git リポジトリに同期され、
> 思考し、行動し、記憶し、進化します。

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Cloud Memory            │
         │              Git Repo                │
         │     diaries/ cognitions/ prompts/     │
         └──────┬──────────┬──────────┬─────────┘
                │          │          │
         ┌──────▼────┐ ┌───▼──────┐ ┌▼──────────┐
         │  🖥️  PC    │ │ 🍓  SBC  │ │ ☁️  Server │
         └───────────┘ └──────────┘ └───────────┘

      同じ魂。違う体。同期された記憶。
```

---

## ⚡ 機能

|   | 機能 | 説明 |
|---|------|------|
| 🔄 | **マルチターミナル** | 一つのエージェント、複数のマシン — シームレスに切り替え |
| ☁️ | **Git メモリ** | 全ての記憶が Git 経由で自動同期 |
| 🧩 | **プラグインツール** | 自動検出される ROS2 アクションノード |
| 🎛️ | **マルチチャネル** | Web チャット / ROS2 / メール / ターミナル |
| 💭 | **思考モード** | 深い推論とストリーミング思考出力 |
| ⚡ | **スナップインセンサー** | システム状態、メッセージ、カスタム入力 |

---

## 🏗️ アーキテクチャ

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / OpenAI 互換
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (自動検出)           │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · メモリ · ディスク · ネットワーク · machine-id
  │ msg_receive  │   web_chat · ROS2 トピック · メール
  │ input_mgmt   │   スナップショット集約
  └──────────────┘
```

> 3つのパッケージ、無限のツール。ROS2 Actions で接続。

---

## 🚀 クイックスタート

```bash
# 1. システム依存関係
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. クローン & ビルド
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. 設定 (対話型ウィザード)
./config

# 4. 起動
./start
```

> Web チャットは `http://localhost:8080` を開いてください。

---

## 🧬 メモリモデル

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← システムプロンプト (cognitions をライブ参照)
│   └── COMPRESS.md   ← コンテキスト圧縮プロンプト
├── cognitions/
│   ├── SELF.md       ← 私は誰か
│   ├── MASTER.md     ← 私のユーザー
│   ├── METHOD.md     ← 私の働き方
│   └── WORLD.md      ← 私が知っていること
└── diaries/
    └── 20260629.md   ← 今日のログ (LLM 圧縮)
```

---

## 🔧 拡張

**ツールの追加** — 1ファイルで自動検出：

1. `src/cs_output/src/my_tool_node.cpp` を作成
2. `/{agent}/output/my_tool/info` に情報を公開 (transient_local)
3. `/{agent}/output/my_tool` で `ExecuteTool` アクションを提供

**センサーの追加** — 同じパターン：

1. `/{agent}/input/my_sensor/info` に `InputInfo` を公開
2. `/{agent}/input/my_sensor` にデータを公開

> `input_mgmt_node` と `output_mgmt_node` が残りを処理します。

---

## 📊 ステータス

| 指標 | 値 |
|------|-----|
| 最新 | `v0.3.3-Beta` |
| パッケージ | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| ツール | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| センサー | 3 (`system_status` `message_receive` `ros_msg`) |
| ノード | 10+ |

---

<div align="center">

**ROS2 Humble 上に ❤️ を込めて構築**

*"魂はクラウドに、体はあらゆるところに。"*

</div>