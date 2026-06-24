# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](#) | [Español](README_es.md)

ROS2-basierte KI-Agenten-Laufzeitumgebung. Ein Agent, mehrere Terminals, cloud-synchronisierter Speicher.

---

## 🏗️ Architektur

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

Drei Pakete, verbunden über ROS2-Action `/agent_loop/_action/execute_tool`:

| Paket | Rolle |
|-------|------|
| `cs_input` | Sensoren: Systemstatus, Nachrichtenabonnement |
| `cs_core` | Agenten-Schleife (LLM ⇄ Werkzeuge), Git-basierter Speicher |
| `cs_output` | Werkzeuge: Shell, Datei-I/O, Nachrichten, Websuche, Web-Chat |

---

## ⚙️ Funktionsweise

```
  system_status  ──┐
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

## 🚀 Schnellstart

**Voraussetzungen**: Ubuntu 22.04, ROS2 Humble, DeepSeek API-Schlüssel (oder kompatibler Endpunkt).

```bash
# Systemabhängigkeiten
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail

# Klonen & Bauen
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Speicher-Repo** — fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), SSH-Push einrichten.

**Starten** (mit Umgebungsvariablen):

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

# 1) Speicher starten
ros2 launch cs_core cs_core.launch.py \
  agent_name:=agent \
  repo_url:=git@github.com:your-org/cloud-soul-memory.git \
  repo_dir:=$HOME/.cloudsoul/soul_repo

# 2) Sensoren starten
ros2 launch cs_input cs_input.launch.py agent_name:=agent

# 3) Werkzeuge starten
ros2 launch cs_output cs_output.launch.py agent_name:=agent

# 4) (Optional) Web-Chat starten
python3 web_chat_server.py --agent agent &
```

**Als root ausführen** (für Systemzugriff):

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

Alle Knoten unterstützen `respawn=True` — ROS2 startet abgestürzte Knoten automatisch neu.

---

## 🧠 Speichermodell

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # System-Prompt (verweist auf [cognitions/*.md])
│   └── COMPRESS.md    # Komprimierungs-Prompt
├── cognitions/
│   ├── SELF.md        # Agenten-Identität
│   ├── MASTER.md      # Benutzerprofil
│   ├── METHOD.md      # Verhaltensregeln
│   └── WORLD.md       # Bekannte Fakten
└── diaries/
    └── YYYYMMDD.md    # Tägliches Protokoll (LLM-komprimiert)
```

---

## 🔌 Knoten

### cs_core

| Knoten | Beschreibung |
|------|-------------|
| `agent_loop_node` | Hauptschleife: Schnappschuss → LLM → Werkzeuge → Wiederholung. Kontextverwaltung |
| `memory_node` | Git Recall/Archivierung: Kognitionen pullen, Tagebücher pushen |

### cs_input

| Knoten | Topic | Beschreibung |
|------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, RAM, Festplatte, Netzwerk, Hostname, Machine-ID (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | ROS2 / Web-Chat Nachrichtenempfang |
| `input_mgmt_node` | `/{agent}/input` (srv) | Aggregation der Sensordaten zum Schnappschuss |

### cs_output

| Knoten | Action | Beschreibung |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Shell-Befehlsausführung |
| `file_read_node` | execute_tool | Datei lesen (Offset/Länge/Kodierung) |
| `file_write_node` | execute_tool | Datei schreiben (Überschreiben/Anhängen) |
| `message_send_node` | execute_tool | E-Mail (s-nail), ROS2-Publish, Web-Chat-Antwort |
| `web_search_node` | execute_tool | Websuche mit Multi-Engine-Fallback |
| `web_chat_server.py` | (Flask SSE) | Browser-Chat-Interface (Port 8080) |
| `output_mgmt_node` | execute_tool | Automatische Erkennung und Weiterleitung an Werkzeuge |

---

## 🔧 Konfiguration

| Parameter | Knoten | Standard |
|-----------|------|---------|
| `agent_name` | alle | `agent` |
| `repo_url` | memory_node | — erforderlich |
| `repo_dir` | memory_node | `~/.cloudsoul/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `tool_timeout` | cs_core / cs_output | `60.0` |
| `info_timeout` | input/output mgmt | `3.0` |

Umgebungsvariablen: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## 🎨 Anpassung

1. Fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), bearbeiten Sie `cognitions/`
2. Werkzeuge hinzufügen: Knoten in `cs_output/src/` erstellen, der `/{agent}/output/<name>/info` (JSON-Werkzeugbeschreibung, `std_msgs/String`, QoS transient_local) publiziert. `output_mgmt_node` erkennt ihn automatisch
3. Sensoren hinzufügen: Knoten in `cs_input/src/` erstellen, der `/{agent}/input/<name>/info` (`InputInfo`-Nachricht, QoS transient_local) und Daten `/{agent}/input/<name>` publiziert. `input_mgmt_node` erkennt ihn automatisch
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — alle Timeouts, Fehlercodes und Nachrichtentexte zentral verwaltet

---

## 📄 Lizenz

MIT