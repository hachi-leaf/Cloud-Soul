# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](#) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](README_es.md)

Среда выполнения AI-агента на базе ROS2. Один агент, множество терминалов, облачная синхронизация памяти.

---

## 🏗️ Архитектура

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

Три пакета, соединённых через ROS2 action `/agent_loop/_action/execute_tool`:

| Пакет | Назначение |
|-------|-----------|
| `cs_input` | Сенсоры: состояние системы, подписка на сообщения |
| `cs_core` | Цикл агента (LLM ⇄ инструменты), память на основе Git |
| `cs_output` | Инструменты: shell, файловый I/O, сообщения, веб-поиск, веб-чат |

---

## ⚙️ Принцип работы

```
  system_status  ──┐
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

## 🚀 Быстрый старт

**Зависимости**: Ubuntu 22.04, ROS2 Humble, ключ API DeepSeek (или совместимый).

```bash
# системные зависимости
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail

# клонирование и сборка
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Репозиторий памяти** — форк [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), настройте SSH push.

**Запуск** (с переменными окружения):

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

# 1) Запуск памяти
ros2 launch cs_core cs_core.launch.py \
  agent_name:=agent \
  repo_url:=git@github.com:your-org/cloud-soul-memory.git \
  repo_dir:=$HOME/.cloudsoul/soul_repo

# 2) Запуск сенсоров
ros2 launch cs_input cs_input.launch.py agent_name:=agent

# 3) Запуск инструментов
ros2 launch cs_output cs_output.launch.py agent_name:=agent

# 4) (Опционально) Веб-чат
python3 web_chat_server.py --agent agent &
```

**Запуск от root** (для системного доступа):

```bash
sudo -E bash -c 'source /opt/ros/humble/setup.bash && source install/setup.bash && \
  ros2 launch cs_core cs_core.launch.py agent_name:=agent \
  repo_url:=git@github.com:your-org/cloud-soul-memory.git \
  repo_dir:=$HOME/.cloudsoul/soul_repo'

sudo -E bash -c 'source /opt/ros/humble/setup.bash && source install/setup.bash && \
  ros2 launch cs_input cs_input.launch.py agent_name:=agent'

sudo -E bash -c 'source /opt/ros/humble/setup.bash && source install/setup.bash && \
  ros2 launch cs_output cs_output.launch.py agent_name:=agent'
```

Все узлы поддерживают `respawn=True` — при падении ROS2 перезапускает автоматически.

---

## 🧠 Модель памяти

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # системный промпт (ссылки [cognitions/*.md])
│   └── COMPRESS.md    # промпт сжатия
├── cognitions/
│   ├── SELF.md        # идентичность агента
│   ├── MASTER.md      # профиль пользователя
│   ├── METHOD.md      # правила поведения
│   └── WORLD.md       # известные факты
└── diaries/
    └── YYYYMMDD.md    # дневной журнал (сжатие LLM)
```

---

## 🔌 Узлы

### cs_core

| Узел | Описание |
|------|----------|
| `agent_loop_node` | Главный цикл: снимок → LLM → инструменты → повтор. Управление контекстом |
| `memory_node` | Git recall/archive: получение когниций, отправка дневников |

### cs_input

| Узел | Топик | Описание |
|------|-------|----------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, память, диск, сеть, хост, machine-id (1 Гц) |
| `message_receive_node` | `/{agent}/input/message_receive` | Приём сообщений ROS2 / web_chat |
| `input_mgmt_node` | `/{agent}/input` (srv) | Агрегация данных сенсоров в снимок |

### cs_output

| Узел | Действие | Описание |
|------|----------|----------|
| `shell_exec_node` | execute_tool | Выполнение команд shell |
| `file_read_node` | execute_tool | Чтение файлов (смещение/длина/кодировка) |
| `file_write_node` | execute_tool | Запись файлов (перезапись/добавление) |
| `message_send_node` | execute_tool | Почта (s-nail), публикация ROS2, ответ web_chat |
| `web_search_node` | execute_tool | Веб-поиск с fallback на несколько движков |
| `web_chat_server.py` | (Flask SSE) | Браузерный чат-интерфейс (порт 8080) |
| `output_mgmt_node` | execute_tool | Автообнаружение и маршрутизация вызовов к инструментам |

---

## 🔧 Конфигурация

| Параметр | Узел | По умолчанию |
|----------|------|-------------|
| `agent_name` | все | `agent` |
| `repo_url` | memory_node | — обязательно |
| `repo_dir` | memory_node | `~/.cloudsoul/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `tool_timeout` | cs_core / cs_output | `60.0` |
| `info_timeout` | input/output mgmt | `3.0` |

Переменные окружения: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## 🎨 Настройка

1. Форк [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), редактируйте `cognitions/`
2. Добавление инструментов: создайте узел в `cs_output/src/`, публикующий `/{agent}/output/<name>/info` (JSON-дескриптор, `std_msgs/String`, QoS transient_local). `output_mgmt_node` обнаружит автоматически
3. Добавление сенсоров: создайте узел в `cs_input/src/`, публикующий `/{agent}/input/<name>/info` (сообщение `InputInfo`, QoS transient_local) и данные `/{agent}/input/<name>`. `input_mgmt_node` обнаружит автоматически
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — все таймауты, коды ошибок и строки сообщений в одном месте

---

## 📄 Лицензия

MIT