# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](#) | [Deutsch](README_de.md) | [Español](README_es.md)

Runtime d'agent IA basé sur ROS2. Un agent, plusieurs terminaux, mémoire synchronisée dans le cloud.

---

## Architecture

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
│ input_mgmt │    │ Dépôt Git ↔  │    │ output_mgmt                  │
└────────────┘    │  cognitions  │    └─────────────────────────────┘
                  │  diaries     │
                  └──────────────┘
```

Trois paquets, connectés via l'action ROS2 `/agent_loop/_action/execute_tool` :

| Paquet | Rôle |
|--------|------|
| `cs_input` | Capteurs : état système, abonnement aux messages |
| `cs_core` | Boucle agent (LLM ⇄ outils), mémoire basée sur Git |
| `cs_output` | Outils : shell, E/S fichiers, messagerie, recherche web |

---

## Fonctionnement

```
  system_status ──┐
  message_receive ─┤
                   ├──→ input_mgmt (instantané) ──→ agent_loop
                   │                                   │
                   │     ┌─────────────────────────────┘
                   │     ▼
                   │   LLM: raisonnement → appels d'outils
                   │     │
                   │     ▼
                   │   output_mgmt → tool_node → résultat
                   │     │
                   │     ▼
                   │   memory_node: archivage dans journal Git
                   │     │
                   └─────┘ (cycle suivant)
```

- Message utilisateur → traitement immédiat
- Inactif → boucle sur heartbeat system_status
- Contexte > seuil → LLM compresse dans le journal, réinitialise
- Mémoire persistée en Markdown dans le dépôt Git ; pull/push par cycle

---

## Démarrage rapide

**Prérequis** : Ubuntu 22.04, ROS2 Humble, clé API DeepSeek (ou endpoint compatible).

```bash
# dépendances
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev

# compilation
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Dépôt mémoire** — fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), configurez SSH push.

**Lancement** :

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

## Modèle de mémoire

```
~/.adam/soul_repo/
├── prompts/
│   ├── RULE.md        # prompt système (réf. [cognitions/*.md])
│   └── COMPRESS.md    # prompt de compression
├── cognitions/
│   ├── SELF.md        # identité de l'agent
│   ├── MASTER.md      # profil utilisateur
│   ├── METHOD.md      # règles comportementales
│   └── WORLD.md       # faits connus
└── diaries/
    └── YYYYMMDD.md    # journal (compressé par LLM)
```

---

## Nœuds

### cs_core

| Nœud | Description |
|------|-------------|
| `agent_loop_node` | Boucle principale : instantané → LLM → outils → répéter |
| `memory_node` | Mémoire Git : pull cognitions, push diaries |

### cs_input

| Nœud | Topic | Description |
|------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, RAM, disque, réseau, hôte, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | Abonnement ROS2 String |
| `input_mgmt_node` | `/{agent}/input/snapshot` (srv) | Agrégation des données capteurs |

### cs_output

| Nœud | Action | Description |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Exécution de commandes shell |
| `file_read_node` | execute_tool | Lecture de fichiers |
| `file_write_node` | execute_tool | Écriture de fichiers |
| `message_send_node` | execute_tool | Email (s-nail) ou publication ROS2 |
| `web_search_node` | execute_tool | Requêtes HTTP, multi-sources |
| `output_mgmt_node` | execute_tool | Routage des appels d'outils |

---

## Configuration

| Paramètre | Nœud | Défaut |
|-----------|------|--------|
| `agent_name` | tous | `adam` |
| `repo_url` | memory_node | — requis |
| `repo_dir` | memory_node | `~/.adam/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `topic_name` | message_receive_node | `/adam/input/message_receive` |

Variables d'env : `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## Personnalisation

1. Fork [Adam-Soul](https://github.com/hachi-leaf/Adam-Soul), éditez `cognitions/`
2. Outils : nouveau nœud dans `cs_output/src/`, enregistrez dans `output_mgmt_node.cpp`
3. Capteurs : nouveau nœud dans `cs_input/src/`, enregistrez dans `input_mgmt_node.cpp`

---

## Licence

MIT