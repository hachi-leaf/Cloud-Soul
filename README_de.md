# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](#) | [Español](README_es.md)

ROS2-basierte KI-Agenten-Laufzeitumgebung. Ein Agent, mehrere Endgeräte, Cloud-synchronisierter Speicher.

---

## Architektur

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
│ input_mgmt │    │ Git-Repo ↔   │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Drei Pakete, verbunden über ROS2-Action `/agent_loop/_action/execute_tool`:

| Paket | Rolle |
|-------|------|
| `cs_input` | Sensoren: Systemstatus, Nachrichtenabonnement |
| `cs_core` | Agenten-Schleife (LLM ⇄ Werkzeuge), Git-basierter Speicher |
| `cs_output` | Werkzeuge: Shell, Datei-I/O, Nachrichten, Websuche |

---

## Funktionsweise

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (Schnappschuss) ──→ agent_loop
                   │                                      │
                   │     ┌────────────────────────────────┘
                   │     ▼
                   │   LLM: Schlussfolgerung → Werkzeugaufrufe
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → Ergebnis
                   │     │
                   │     ▼
                   │   memory_node: Archivierung in Git-Tagebuch
                   │     │
                   └─────┘ (nächster Zyklus)
```

- Benutzernachricht → sofortige Verarbeitung
- Leerlauf → Schleife über system_status-Heartbeat
- Kontext überschreitet Schwelle → LLM komprimiert ins Tagebuch, setzt zurück
- Speicher als Markdown im Git-Repo; pull/push pro Zyklus

---

## Schnellstart

**Voraussetzungen**: Ubuntu 22.04, ROS2 Humble, DeepSeek API-Schlüssel (oder kompatibler Endpunkt).

```bash
# Abhängigkeiten
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# Bauen
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Speicher-Repo** — fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), SSH-Push einrichten.

**Starten**:

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

## Speichermodell

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # System-Prompt (verweist auf [cognitions/*.md])
│   └── COMPRESS.md    # Komprimierungs-Prompt
├── cognitions/
│   ├── SELF.md        # Agentenidentität
│   ├── MASTER.md      # Benutzerprofil
│   ├── METHOD.md      # Verhaltensregeln
│   └── WORLD.md       # Bekannte Fakten
└── diaries/
    └── YYYYMMDD.md    # Tagebuch (LLM-komprimiert)
```

---

## Knoten

### cs_core

| Knoten | Beschreibung |
|--------|-------------|
| `agent_loop_node` | Hauptschleife: Schnappschuss → LLM → Werkzeuge → Wiederholung |
| `memory_node` | Git-Speicher: pull cognitions, push diaries |

### cs_input

| Knoten | Topic | Beschreibung |
|--------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, RAM, Festplatte, Netzwerk, Host, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 String Abonnement |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | Sensordaten aggregieren |

### cs_output

| Knoten | Action | Beschreibung |
|--------|--------|-------------|
| `shell_exec_node` | execute_tool | Shell-Befehle ausführen |
| `file_read_node` | execute_tool | Dateien lesen |
| `file_write_node` | execute_tool | Dateien schreiben |
| `message_send_node` | execute_tool | E-Mail (s-nail) oder ROS2-Veröffentlichung |
| `web_search_node` | execute_tool | HTTP-Anfragen, Multi-Quellen |
| `output_mgmt_node` | execute_tool | Werkzeugaufrufe weiterleiten |

---

## Konfiguration

| Parameter | Knoten | Standard |
|-----------|--------|---------|
| `agent_name` | alle | `adam` |
| `repo_url` | memory_node | — erforderlich |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

Umgebungsvariablen: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## Anpassung

1. Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), bearbeiten Sie `cognitions/`
2. Werkzeuge: neuer Knoten in `cs_output/src/`, registrieren in `output_mgmt_node.cpp`
3. Sensoren: neuer Knoten in `cs_input/src/`, registrieren in `input_mgmt_node.cpp`

---

## Lizenz

MIT