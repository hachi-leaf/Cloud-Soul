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

## 🏗️ Arquitectura

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

Tres paquetes, conectados mediante la acción ROS2 `/agent_loop/_action/execute_tool`:

| Paquete | Función |
|---------|---------|
| `cs_input` | Sensores: estado del sistema, suscripción a mensajes |
| `cs_core` | Bucle del agente (LLM ⇄ herramientas), memoria basada en Git |
| `cs_output` | Herramientas: shell, E/S de archivos, mensajería, búsqueda web, chat web |

---

## ⚙️ Funcionamiento

```
  system_status  ──┐
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

## 🚀 Inicio rápido

**Requisitos**: Ubuntu 22.04, ROS2 Humble, clave API DeepSeek (o endpoint compatible).

```bash
# dependencias del sistema
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail

# clonar y compilar
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Repo de memoria** — fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), configure SSH push.

**Lanzamiento** (con variables de entorno):

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

# 1) Iniciar memoria
ros2 launch cs_core cs_core.launch.py \
  agent_name:=agent \
  repo_url:=git@github.com:your-org/cloud-soul-memory.git \
  repo_dir:=$HOME/.cloudsoul/soul_repo

# 2) Iniciar sensores
ros2 launch cs_input cs_input.launch.py agent_name:=agent

# 3) Iniciar herramientas
ros2 launch cs_output cs_output.launch.py agent_name:=agent

# 4) (Opcional) Chat web
python3 web_chat_server.py --agent agent &
```

**Ejecutar como root** (para acceso a nivel de sistema):

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

Todos los nodos soportan `respawn=True` — ROS2 reinicia automáticamente los nodos caídos.

---

## 🧠 Modelo de memoria

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # prompt del sistema (ref [cognitions/*.md])
│   └── COMPRESS.md    # prompt de compresión
├── cognitions/
│   ├── SELF.md        # identidad del agente
│   ├── MASTER.md      # perfil de usuario
│   ├── METHOD.md      # reglas de comportamiento
│   └── WORLD.md       # hechos conocidos
└── diaries/
    └── YYYYMMDD.md    # registro diario (comprimido por LLM)
```

---

## 🔌 Nodos

### cs_core

| Nodo | Descripción |
|------|-------------|
| `agent_loop_node` | Bucle principal: instantánea → LLM → herramientas → repetir. Gestión de contexto |
| `memory_node` | Git recall/archive: obtener cogniciones, enviar diarios |

### cs_input

| Nodo | Tópico | Descripción |
|------|--------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, RAM, disco, red, hostname, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | Recepción de mensajes ROS2 / web_chat |
| `input_mgmt_node` | `/{agent}/input` (srv) | Agregación de datos de sensores en instantánea |

### cs_output

| Nodo | Acción | Descripción |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Ejecución de comandos shell |
| `file_read_node` | execute_tool | Lectura de archivos (offset/longitud/codificación) |
| `file_write_node` | execute_tool | Escritura de archivos (sobrescribir/añadir) |
| `message_send_node` | execute_tool | Email (s-nail), publicación ROS2, respuesta web_chat |
| `web_search_node` | execute_tool | Búsqueda web con fallback multi-motor |
| `web_chat_server.py` | (Flask SSE) | Interfaz de chat en navegador (puerto 8080) |
| `output_mgmt_node` | execute_tool | Descubrimiento automático y enrutamiento a herramientas |

---

## 🔧 Configuración

| Parámetro | Nodo | Predeterminado |
|-----------|------|----------------|
| `agent_name` | todos | `agent` |
| `repo_url` | memory_node | — requerido |
| `repo_dir` | memory_node | `~/.cloudsoul/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `tool_timeout` | cs_core / cs_output | `60.0` |
| `info_timeout` | input/output mgmt | `3.0` |

Variables de entorno: `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## 🎨 Personalización

1. Fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), edite `cognitions/`
2. Añadir herramientas: cree un nodo en `cs_output/src/` que publique `/{agent}/output/<name>/info` (descriptor JSON, `std_msgs/String`, QoS transient_local). `output_mgmt_node` lo descubre automáticamente
3. Añadir sensores: cree un nodo en `cs_input/src/` que publique `/{agent}/input/<name>/info` (mensaje `InputInfo`, QoS transient_local) y datos `/{agent}/input/<name>`. `input_mgmt_node` lo descubre automáticamente
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — todos los timeouts, códigos de error y mensajes centralizados

---

## 📄 Licencia

MIT