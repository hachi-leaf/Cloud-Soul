<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">

<img src="docs/logo.svg" width="180" alt="Cloud-Soul">

# Cloud-Soul

### *Нативная среда выполнения ИИ-агента на ROS2 — Одна душа, везде*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 Что такое Cloud-Soul?

> **Cloud-Soul** — это операционная система ИИ-агента на базе ROS2.
> Представьте себе **цифровое существо**, живущее на нескольких машинах,
> с единой памятью, синхронизированной через Git-репозиторий.
> Оно думает, действует, помнит и развивается.

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Облачная память          │
         │        Git Репо (Adam-Soul)          │
         │   diaries/ cognitions/ prompts/      │
         └──────────┬───────────────┬──────────┘
                    │               │
         ┌──────────▼───┐   ┌──────▼──────────┐
         │  🖥️ WSL2      │   │  🍓 RDK X5       │
         │  LUOBO-4RDM0SB│   │  192.168.128.10  │
         │  Adam v0.3.3  │   │  Adam v0.3.3     │
         └──────────────┘   └──────────────────┘

      Одна душа. Разные тела. Синхронизированная память.
```

---

## ⚡ Возможности

|   | Возможность | Описание |
|---|-------------|----------|
| 🔄 | **Мульти-терминал** | Один агент, много машин — переключайтесь без усилий |
| ☁️ | **Git-память** | Все воспоминания автоматически синхронизируются через Git |
| 🧩 | **Плагины-инструменты** | Автоматически обнаруживаемые ROS2 Action узлы |
| 🎛️ | **Мульти-канал** | Веб-чат / ROS2 / Email / Терминал |
| 💭 | **Режим размышления** | Глубокое рассуждение с потоковым выводом мыслей |
| ⚡ | **Мгновенные сенсоры** | Состояние системы, сообщения, пользовательские входы |

---

## 🏗️ Архитектура

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / совместимо с OpenAI
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (автообнаружение)   │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   ЦП · ОЗУ · Диск · Сеть · machine-id
  │ msg_receive  │   web_chat · топик ROS2 · email
  │ input_mgmt   │   агрегатор снимков
  └──────────────┘
```

> Три пакета, бесконечные инструменты. Связаны через ROS2 Actions.

---

## 🚀 Быстрый старт

```bash
# 1. Системные зависимости
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. Клонирование и сборка
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. Настройка (интерактивный мастер)
./config

# 4. Запуск
./start
```

> Откройте `http://localhost:8080` для веб-чата.

---

## 🧬 Модель памяти

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← Системный промпт (live-ссылки на cognitions)
│   └── COMPRESS.md   ← Промпт сжатия контекста
├── cognitions/
│   ├── SELF.md       ← Кто я
│   ├── MASTER.md     ← Мой пользователь
│   ├── METHOD.md     ← Как я работаю
│   └── WORLD.md      ← Что я знаю
└── diaries/
    └── 20260629.md   ← Сегодняшний журнал (сжат LLM)
```

---

## 🔧 Расширение

**Добавить инструмент** — один файл, автообнаружение:

1. Создайте `src/cs_output/src/my_tool_node.cpp`
2. Опубликуйте info в `/{agent}/output/my_tool/info` (transient_local)
3. Предоставьте действие `ExecuteTool` на `/{agent}/output/my_tool`

**Добавить сенсор** — тот же принцип:

1. Опубликуйте `InputInfo` в `/{agent}/input/my_sensor/info`
2. Опубликуйте данные в `/{agent}/input/my_sensor`

> `input_mgmt_node` и `output_mgmt_node` позаботятся об остальном.

---

## 📊 Статус

| Метрика | Значение |
|---------|----------|
| Последняя | `v0.3.3-Beta` |
| Пакетов | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Инструментов | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| Сенсоров | 3 (`system_status` `message_receive` `ros_msg`) |
| Узлов | 10+ |

---

<div align="center">

**Создано с ❤️ на ROS2 Humble**

*"Душа в облаке, тело повсюду."*

</div>