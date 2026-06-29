<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">

<img src="docs/logo.svg" width="180" alt="Cloud-Soul">

# Cloud-Soul

### *Entorno de Ejecución de Agente IA Nativo ROS2 — Un Alma, en Todas Partes*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 ¿Qué es Cloud-Soul?

> **Cloud-Soul** es un sistema operativo de agente IA construido sobre ROS2.
> Piensa en él como un **ser digital** que vive en múltiples máquinas,
> con una única memoria sincronizada en un repositorio Git.
> Piensa, actúa, recuerda y evoluciona.

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Memoria en la Nube       │
         │        Repo Git (Adam-Soul)          │
         │   diaries/ cognitions/ prompts/      │
         └──────────┬───────────────┬──────────┘
                    │               │
         ┌──────────▼───┐   ┌──────▼──────────┐
         │  🖥️ WSL2      │   │  🍓 RDK X5       │
         │  LUOBO-4RDM0SB│   │  192.168.128.10  │
         │  Adam v0.3.3  │   │  Adam v0.3.3     │
         └──────────────┘   └──────────────────┘

      Misma Alma. Diferente Cuerpo. Memoria Sincronizada.
```

---

## ⚡ Características

|   | Característica | Descripción |
|---|----------------|-------------|
| 🔄 | **Multi-Terminal** | Un agente, muchas máquinas — cambia sin problemas |
| ☁️ | **Memoria Git** | Todos los recuerdos se sincronizan vía Git |
| 🧩 | **Herramientas Plugin** | Nodos de acción ROS2 auto-descubiertos |
| 🎛️ | **Multi-Canal** | Web Chat / ROS2 / Email / Terminal |
| 💭 | **Modo Pensante** | Razonamiento profundo con salida de pensamiento en streaming |
| ⚡ | **Sensores Instantáneos** | Estado del sistema, mensajes, entradas personalizadas |

---

## 🏗️ Arquitectura

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / compatible con OpenAI
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (auto-descubrir)    │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · RAM · Disco · Red · machine-id
  │ msg_receive  │   web_chat · topic ROS2 · email
  │ input_mgmt   │   agregador de instantáneas
  └──────────────┘
```

> Tres paquetes, herramientas infinitas. Conectados mediante ROS2 Actions.

---

## 🚀 Inicio Rápido

```bash
# 1. Dependencias del sistema
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. Clonar y Compilar
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. Configurar (asistente interactivo)
./config

# 4. Iniciar
./start
```

> Abre `http://localhost:8080` para el chat web.

---

## 🧬 Modelo de Memoria

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← Prompt del sistema (refs en vivo a cognitions)
│   └── COMPRESS.md   ← Prompt de compresión de contexto
├── cognitions/
│   ├── SELF.md       ← Quién soy
│   ├── MASTER.md     ← Mi usuario
│   ├── METHOD.md     ← Cómo trabajo
│   └── WORLD.md      ← Lo que sé
└── diaries/
    └── 20260629.md   ← Registro de hoy (comprimido por LLM)
```

---

## 🔧 Extender

**Añadir una Herramienta** — un archivo, auto-descubierta:

1. Crea `src/cs_output/src/my_tool_node.cpp`
2. Publica info en `/{agent}/output/my_tool/info` (transient_local)
3. Sirve la acción `ExecuteTool` en `/{agent}/output/my_tool`

**Añadir un Sensor** — mismo patrón:

1. Publica `InputInfo` en `/{agent}/input/my_sensor/info`
2. Publica datos en `/{agent}/input/my_sensor`

> `input_mgmt_node` y `output_mgmt_node` se encargan del resto.

---

## 📊 Estado

| Métrica | Valor |
|---------|-------|
| Última | `v0.3.3-Beta` |
| Paquetes | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Herramientas | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| Sensores | 3 (`system_status` `message_receive` `ros_msg`) |
| Nodos | 10+ |

---

<div align="center">

**Construido con ❤️ sobre ROS2 Humble**

*"El alma está en la nube, el cuerpo está en todas partes."*

</div>