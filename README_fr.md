# Cloud-Soul

<p align="left">
  <img src="https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros" alt="ROS2">
  <img src="https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/MIT-License-97CA00" alt="MIT">
  <img src="https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu" alt="Ubuntu">
</p>

🌐 [English](README.md) | [中文](README_zh.md) | [日本語](README_ja.md) | [Русский](README_ru.md) | [Français](#) | [Deutsch](README_de.md) | [Español](README_es.md)

Environnement d'exécution d'agent IA basé sur ROS2. Un agent, plusieurs terminaux, mémoire synchronisée dans le cloud.

---

## 🏗️ Architecture

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

Trois paquets, connectés via l'action ROS2 `/agent_loop/_action/execute_tool` :

| Paquet | Rôle |
|--------|------|
| `cs_input` | Capteurs : état système, abonnement aux messages |
| `cs_core` | Boucle agent (LLM ⇄ outils), mémoire basée sur Git |
| `cs_output` | Outils : shell, E/S fichiers, messagerie, recherche web, chat web |

---

## ⚙️ Fonctionnement

```
  system_status  ──┐
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

## 🚀 Démarrage rapide

**Prérequis** : Ubuntu 22.04, ROS2 Humble, clé API DeepSeek (ou endpoint compatible).

```bash
# dépendances système
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail

# clone & compilation
git clone git@github.com:your-org/Cloud-Soul.git
cd Cloud-Soul
colcon build --symlink-install
```

**Dépôt mémoire** — fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), configurez SSH push.

**Lancement** (avec variables d'environnement) :

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

export OPENAI_API_KEY=sk-xxx
export OPENAI_BASE_URL=https://api.deepseek.com/v1
export OPENAI_MODEL=deepseek-chat

# 1) Démarrer la mémoire
ros2 launch cs_core cs_core.launch.py \
  agent_name:=agent \
  repo_url:=git@github.com:your-org/cloud-soul-memory.git \
  repo_dir:=$HOME/.cloudsoul/soul_repo

# 2) Démarrer les capteurs
ros2 launch cs_input cs_input.launch.py agent_name:=agent

# 3) Démarrer les outils
ros2 launch cs_output cs_output.launch.py agent_name:=agent

# 4) (Optionnel) Chat web
python3 web_chat_server.py --agent agent &
```

**Exécution en root** (pour accès système) :

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

Tous les nœuds supportent `respawn=True` — redémarrage automatique par ROS2.

---

## 🧠 Modèle de mémoire

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md        # prompt système (références [cognitions/*.md])
│   └── COMPRESS.md    # prompt de compression
├── cognitions/
│   ├── SELF.md        # identité de l'agent
│   ├── MASTER.md      # profil utilisateur
│   ├── METHOD.md      # règles de comportement
│   └── WORLD.md       # faits connus
└── diaries/
    └── YYYYMMDD.md    # journal quotidien (compressé par LLM)
```

---

## 🔌 Nœuds

### cs_core

| Nœud | Description |
|------|-------------|
| `agent_loop_node` | Boucle principale : instantané → LLM → outils → répéter. Gestion du contexte |
| `memory_node` | Rappel/archivage Git : pull des cognitions, push des journaux |

### cs_input

| Nœud | Topic | Description |
|------|-------|-------------|
| `system_status_node` | `/{agent}/input/system_status` | CPU, mémoire, disque, réseau, hôte, machine-id (1 Hz) |
| `message_receive_node` | `/{agent}/input/message_receive` | Réception messages ROS2 / web_chat |
| `input_mgmt_node` | `/{agent}/input` (srv) | Agrégation des données capteurs en instantané |

### cs_output

| Nœud | Action | Description |
|------|--------|-------------|
| `shell_exec_node` | execute_tool | Exécution de commandes shell |
| `file_read_node` | execute_tool | Lecture de fichiers (offset/longueur/encodage) |
| `file_write_node` | execute_tool | Écriture de fichiers (écrasement/ajout) |
| `message_send_node` | execute_tool | Email (s-nail), publication ROS2, réponse web_chat |
| `web_search_node` | execute_tool | Recherche web avec fallback multi-moteurs |
| `web_chat_server.py` | (Flask SSE) | Interface chat navigateur (port 8080) |
| `output_mgmt_node` | execute_tool | Découverte automatique et routage vers les outils |

---

## 🔧 Configuration

| Paramètre | Nœud | Défaut |
|-----------|------|--------|
| `agent_name` | tous | `agent` |
| `repo_url` | memory_node | — requis |
| `repo_dir` | memory_node | `~/.cloudsoul/soul_repo` |
| `max_context_tokens` | agent_loop_node | `200000` |
| `summary_turns` | agent_loop_node | `30` |
| `tool_timeout` | cs_core / cs_output | `60.0` |
| `info_timeout` | input/output mgmt | `3.0` |

Variables d'environnement : `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OPENAI_MODEL`.

---

## 🎨 Personnalisation

1. Fork [cloud-soul-memory](https://github.com/your-org/cloud-soul-memory), éditez `cognitions/`
2. Ajout d'outils : créez un nœud dans `cs_output/src/` publiant `/{agent}/output/<name>/info` (descripteur JSON, `std_msgs/String`, QoS transient_local). `output_mgmt_node` le découvre automatiquement
3. Ajout de capteurs : créez un nœud dans `cs_input/src/` publiant `/{agent}/input/<name>/info` (message `InputInfo`, QoS transient_local) et les données `/{agent}/input/<name>`. `input_mgmt_node` le découvre automatiquement
4. `cs_interfaces/include/cs_interfaces/constants.hpp` — tous les timeouts, codes d'erreur et messages au même endroit

---

## 📄 Licence

MIT