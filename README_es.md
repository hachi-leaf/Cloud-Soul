# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](README_fr.md) | [Deutsch](README_de.md) | [Español](#)

Entorno de ejecución de agente IA basado en ROS2. Un agente, múltiples terminales, memoria sincronizada en la nube.

---

## Arquitectura

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
│ input_mgmt │    │ Repo Git ↔   │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Tres paquetes, conectados mediante la acción ROS2 `/agent_loop/_action/execute_tool`:

| Paquete | Función |
|---------|---------|
| `cs_input` | Sensores: estado del sistema, suscripción a mensajes |
| `cs_core` | Bucle del agente (LLM ⇄ herramientas), memoria basada en Git |
| `cs_output` | Herramientas: shell, E/S de archivos, mensajería, búsqueda web |

---

## Funcionamiento

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (instantánea) ──→ agent_loop
                   │                                    │
                   │     ┌──────────────────────────────┘
                   │     ▼
                   │   LLM: razonamiento → llamadas a herramientas
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → resultado
                   │     │
                   │     ▼
                   │   memory_node: archivado en diario Git
                   │     │
                   └─────┘ (siguiente ciclo)
```

- Mensaje de usuario → procesamiento inmediato
- Inactivo → bucle sobre heartbeat system_status
- Contexto supera el umbral → LLM comprime en diario, reinicia
- Memoria persistida como Markdown en repo Git; pull/push por ciclo

---

## Inicio rápido

**Requisitos**: Ubuntu 22.04, ROS2 Humble, clave API DeepSeek (o endpoint compatible).

```bash
# dependencias
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# compilación
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Repo de memoria** — fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), configure SSH push.

**Lanzamiento**:

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

## Modelo de memoria

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # prompt del sistema (ref. [cognitions/*.md])
│   └── COMPRESS.md    # prompt de compresión
├── cognitions/
│   ├── SELF.md        # identidad del agente
│   ├── MASTER.md      # perfil del usuario
│   ├── METHOD.md      # reglas de comportamiento
│   └── WORLD.md       # hechos conocidos
└── diaries/
    └── YYYYMMDD.md    # diario (comprimido por LLM)
```

---

## Nodos

### cs_core

| Nodo | Descripción |
|------|-------------|
| `agent_loop_node` | Bucle principal: instantánea → LLM → herramientas → repetir |
| `memory_node` | Memoria Git: pull cognitions, push diaries |

### cs_input

| Nodo | Tópico | Descripción |
|------|--------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, RAM, disco, red, host, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | Suscripción ROS2 String |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | Agregación de datos de sensores |

### cs_output

| Nodo | Acción | Descripción |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Ejecución de comandos shell |
| `file_read_node` | execute_tool | Lectura de archivos |
| `file_write_node` | execute_tool | Escritura de archivos |
| `message_send_node` | execute_tool | Email (s-nail) o publicación ROS2 |
| `web_search_node` | execute_tool | Peticiones HTTP, múltiples fuentes |
| `output_mgmt_node` | execute_tool | Enrutamiento de llamadas a herramientas |

---

## Configuración

| Parámetro | Nodo | Valor por defecto |
|-----------|------|-------------------|
| `agent_name` | todos | `adam` |
| `repo_url` | memory_node | — requerido |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

Variables de entorno: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## Personalización

1. Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), edite `cognitions/`
2. Herramientas: nuevo nodo en `cs_output/src/`, registrar en `output_mgmt_node.cpp`
3. Sensores: nuevo nodo en `cs_input/src/`, registrar en `input_mgmt_node.cpp`

---

## Licencia

MIT