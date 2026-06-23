# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](#) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

Среда выполнения AI-агента на базе ROS2. Один агент, несколько терминалов, облачная синхронизация памяти.

---

## Архитектура

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
│ input_mgmt │    │ Git репо ↔   │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Три пакета, соединённых через ROS2 action `/agent_loop/_action/execute_tool`:

| Пакет | Назначение |
|-------|-----------|
| `cs_input` | Сенсоры: состояние системы, подписка на сообщения |
| `cs_core` | Цикл агента (LLM ⇄ инструменты), память на основе Git |
| `cs_output` | Инструменты: shell, файловый I/O, сообщения, веб-поиск |

---

## Принцип работы

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (снимок) ──→ agent_loop
                   │                               │
                   │     ┌─────────────────────────┘
                   │     ▼
                   │   LLM: рассуждение → вызов инструментов
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → результат
                   │     │
                   │     ▼
                   │   memory_node: архивация в Git-дневник
                   │     │
                   └─────┘ (следующий цикл)
```

- Сообщение пользователя → немедленная обработка
- Ожидание → цикл по heartbeat system_status
- Превышение порога контекста → LLM сжимает в дневник, сбрасывает
- Память хранится в Markdown в Git-репо; pull/push каждый цикл

---

## Быстрый старт

**Требования**: Ubuntu 22.04, ROS2 Humble, ключ API DeepSeek (или совместимый).

```bash
# зависимости
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# сборка
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Репо памяти** — форк [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), настройте SSH push.

**Запуск**:

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

## Модель памяти

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # системный промпт (ссылается на [cognitions/*.md])
│   └── COMPRESS.md    # промпт сжатия
├── cognitions/
│   ├── SELF.md        # идентичность агента
│   ├── MASTER.md      # профиль пользователя
│   ├── METHOD.md      # правила поведения
│   └── WORLD.md       # известные факты
└── diaries/
    └── YYYYMMDD.md    # дневник (сжато LLM)
```

---

## Узлы

### cs_core

| Узел | Описание |
|------|----------|
| `agent_loop_node` | Главный цикл: снимок → LLM → инструменты → повтор |
| `memory_node` | Git-память: pull cognitions, push diaries |

### cs_input

| Узел | Топик | Описание |
|------|-------|----------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, память, диск, сеть, хост, machine-id (1 Гц) |
| `message_receive_node` | `/{agent}/input/message_receive` | Подписка на ROS2 String |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | Агрегация данных сенсоров |

### cs_output

| Узел | Action | Описание |
|------|--------|----------|
| `shell_exec_node` | execute_tool | Выполнение команд shell |
| `file_read_node` | execute_tool | Чтение файлов |
| `file_write_node` | execute_tool | Запись файлов |
| `message_send_node` | execute_tool | Email (s-nail) или ROS2 публикация |
| `web_search_node` | execute_tool | HTTP-запросы, мульти-источник |
| `output_mgmt_node` | execute_tool | Маршрутизация вызовов инструментов |

---

## Конфигурация

| Параметр | Узел | По умолчанию |
|----------|------|-------------|
| `agent_name` | все | `adam` |
| `repo_url` | memory_node | — обязательно |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

Переменные среды: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## Настройка

1. Форк [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), редактируйте `cognitions/`
2. Инструменты: новый узел в `cs_output/src/`, регистрация в `output_mgmt_node.cpp`
3. Сенсоры: новый узел в `cs_input/src/`, регистрация в `input_mgmt_node.cpp`

---

## Лицензия

MIT