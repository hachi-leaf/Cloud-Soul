<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">


# Cloud-Soul

### *ROS2-native AI-Agent-Laufzeitumgebung — Eine Seele, überall*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 Was ist Cloud-Soul?

> **Cloud-Soul** ist ein KI-Agenten-Betriebssystem auf ROS2-Basis.
> Stell es dir als ein **digitales Wesen** vor, das auf mehreren Maschinen lebt,
> mit einem einzigen Gedächtnis, das über ein Git-Repository synchronisiert wird.
> Es denkt, handelt, erinnert sich und entwickelt sich weiter.

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Cloud-Speicher           │
         │        Git-Repo (Adam-Soul)          │
         │   diaries/ cognitions/ prompts/      │
         └──────────┬───────────────┬──────────┘
                    │               │
         ┌──────────▼───┐   ┌──────▼──────────┐
         │  🖥️ WSL2      │   │  🍓 RDK X5       │
         │  LUOBO-4RDM0SB│   │  192.168.128.10  │
         │  Adam v0.3.3  │   │  Adam v0.3.3     │
         └──────────────┘   └──────────────────┘

      Gleiche Seele. Anderer Körper. Synchronisiertes Gedächtnis.
```

---

## ⚡ Funktionen

|   | Funktion | Beschreibung |
|---|----------|--------------|
| 🔄 | **Multi-Terminal** | Ein Agent, viele Maschinen — nahtlos wechseln |
| ☁️ | **Git-Speicher** | Alle Erinnerungen automatisch per Git synchronisiert |
| 🧩 | **Plugin-Tools** | Automatisch erkannte ROS2-Action-Nodes |
| 🎛️ | **Multi-Kanal** | Web-Chat / ROS2 / E-Mail / Terminal |
| 💭 | **Denkmodus** | Tiefes Reasoning mit Streaming-Gedankenausgabe |
| ⚡ | **Snap-In-Sensoren** | Systemstatus, Nachrichten, benutzerdefinierte Eingaben |

---

## 🏗️ Architektur

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / OpenAI-kompatibel
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (Auto-Erkennung)    │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · RAM · Festplatte · Netz · machine-id
  │ msg_receive  │   web_chat · ROS2-Topic · E-Mail
  │ input_mgmt   │   Snapshot-Aggregator
  └──────────────┘
```

> Drei Pakete, unendlich viele Tools. Verbunden über ROS2 Actions.

---

## 🚀 Schnellstart

```bash
# 1. Systemabhängigkeiten
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. Klonen & Bauen
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. Konfigurieren (interaktiver Assistent)
./config

# 4. Starten
./start
```

> Öffne `http://localhost:8080` für den Web-Chat.

---

## 🧬 Speichermodell

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← System-Prompt (live-refs cognitions)
│   └── COMPRESS.md   ← Kontextkomprimierungs-Prompt
├── cognitions/
│   ├── SELF.md       ← Wer ich bin
│   ├── MASTER.md     ← Mein Benutzer
│   ├── METHOD.md     ← Wie ich arbeite
│   └── WORLD.md      ← Was ich weiß
└── diaries/
    └── 20260629.md   ← Heutiges Log (LLM-komprimiert)
```

---

## 🔧 Erweitern

**Tool hinzufügen** — eine Datei, automatisch erkannt:

1. Erstelle `src/cs_output/src/my_tool_node.cpp`
2. Veröffentliche Info an `/{agent}/output/my_tool/info` (transient_local)
3. Biete `ExecuteTool`-Action an `/{agent}/output/my_tool` an

**Sensor hinzufügen** — gleiches Muster:

1. Veröffentliche `InputInfo` an `/{agent}/input/my_sensor/info`
2. Veröffentliche Daten an `/{agent}/input/my_sensor`

> `input_mgmt_node` und `output_mgmt_node` kümmern sich um den Rest.

---

## 📊 Status

| Metrik | Wert |
|--------|------|
| Aktuell | `v0.3.3-Beta` |
| Pakete | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Tools | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| Sensoren | 3 (`system_status` `message_receive` `ros_msg`) |
| Nodes | 10+ |

---

<div align="center">

**Mit ❤️ auf ROS2 Humble gebaut**

*"Die Seele ist in der Cloud, der Körper ist überall."*

</div>