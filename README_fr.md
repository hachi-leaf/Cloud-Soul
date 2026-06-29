<div align="center">

[🇺🇸 English](README.md) | [🇨🇳 中文](README_zh.md) | [🇩🇪 Deutsch](README_de.md) | [🇪🇸 Español](README_es.md) | [🇫🇷 Français](README_fr.md) | [🇯🇵 日本語](README_ja.md) | [🇷🇺 Русский](README_ru.md)

</div>

<div align="center">


# Cloud-Soul

### *Runtime d'Agent IA Natif ROS2 — Une Âme, Partout*

[![ROS2](https://img.shields.io/badge/ROS2-Humble-22314E?logo=ros&style=for-the-badge)](https://docs.ros.org/en/humble/)
[![C++17](https://img.shields.io/badge/C++-17-00599C?logo=c%2B%2B&style=for-the-badge)](https://en.cppreference.com/w/cpp/17)
[![MIT](https://img.shields.io/badge/License-MIT-97CA00?style=for-the-badge)](LICENSE)
[![Ubuntu](https://img.shields.io/badge/Ubuntu-22.04-E95420?logo=ubuntu&style=for-the-badge)](https://releases.ubuntu.com/22.04/)

</div>

---

## 🧬 Qu'est-ce que Cloud-Soul ?

> **Cloud-Soul** est un système d'exploitation pour agent IA construit sur ROS2.
> Imaginez un **être numérique** qui vit sur plusieurs machines,
> avec une mémoire unique synchronisée via un dépôt Git.
> Il pense, agit, se souvient et évolue.

```text
         ┌──────────────────────────────────────┐
         │            ☁️  Mémoire Cloud            │
         │        Dépôt Git (Adam-Soul)         │
         │   diaries/ cognitions/ prompts/      │
         └──────────┬───────────────┬──────────┘
                    │               │
         ┌──────────▼───┐   ┌──────▼──────────┐
         │  🖥️ WSL2      │   │  🍓 RDK X5       │
         │  LUOBO-4RDM0SB│   │  192.168.128.10  │
         │  Adam v0.3.3  │   │  Adam v0.3.3     │
         └──────────────┘   └──────────────────┘

      Même Âme. Corps Différent. Mémoire Synchronisée.
```

---

## ⚡ Fonctionnalités

|   | Fonctionnalité | Description |
|---|----------------|-------------|
| 🔄 | **Multi-Terminal** | Un agent, plusieurs machines — basculez sans effort |
| ☁️ | **Mémoire Git** | Tous les souvenirs synchronisés automatiquement via Git |
| 🧩 | **Outils Plugins** | Nœuds d'action ROS2 auto-découverts |
| 🎛️ | **Multi-Canal** | Chat Web / ROS2 / Email / Terminal |
| 💭 | **Mode Réflexion** | Raisonnement profond avec flux de pensée |
| ⚡ | **Capteurs Instantanés** | État système, messages, entrées personnalisées |

---

## 🏗️ Architecture

```
  ┌─────────────┐
  │  LLM (API)  │  DeepSeek / compatible OpenAI
  └──────┬──────┘
         │
  ┌──────┴───────┐    ┌──────────────────────────────────┐
  │   cs_core    │◄───│          cs_output               │
  │              │    │                                  │
  │ agent_loop   │    │  shell_exec   file_rdwt          │
  │ memory_node  │    │  message_send web_search         │
  │ call_openai  │    │  output_mgmt (auto-découverte)   │
  └──────┬───────┘    └──────────────────────────────────┘
         │
  ┌──────┴───────┐
  │   cs_input   │
  │              │
  │ system_status│   CPU · RAM · Disque · Réseau · machine-id
  │ msg_receive  │   web_chat · topic ROS2 · email
  │ input_mgmt   │   agrégateur d'instantanés
  └──────────────┘
```

> Trois paquets, des outils infinis. Connectés via ROS2 Actions.

---

## 🚀 Démarrage Rapide

```bash
# 1. Dépendances système
sudo apt install ros-humble-desktop libgit2-dev libcurl4-openssl-dev \
  nlohmann-json3-dev libxml2-dev s-nail python3-yaml

# 2. Cloner & Compiler
git clone git@github.com:hachi-leaf/Cloud-Soul.git
cd Cloud-Soul && colcon build --symlink-install

# 3. Configurer (assistant interactif)
./config

# 4. Lancer
./start
```

> Ouvrez `http://localhost:8080` pour le chat web.

---

## 🧬 Modèle de Mémoire

```
~/.cloudsoul/soul_repo/
├── prompts/
│   ├── RULE.md       ← Prompt système (réf live vers cognitions)
│   └── COMPRESS.md   ← Prompt de compression de contexte
├── cognitions/
│   ├── SELF.md       ← Qui je suis
│   ├── MASTER.md     ← Mon utilisateur
│   ├── METHOD.md     ← Comment je travaille
│   └── WORLD.md      ← Ce que je sais
└── diaries/
    └── 20260629.md   ← Journal du jour (compressé par LLM)
```

---

## 🔧 Étendre

**Ajouter un Outil** — un fichier, auto-découvert :

1. Créez `src/cs_output/src/my_tool_node.cpp`
2. Publiez les infos sur `/{agent}/output/my_tool/info` (transient_local)
3. Servez l'action `ExecuteTool` sur `/{agent}/output/my_tool`

**Ajouter un Capteur** — même principe :

1. Publiez `InputInfo` sur `/{agent}/input/my_sensor/info`
2. Publiez les données sur `/{agent}/input/my_sensor`

> `input_mgmt_node` et `output_mgmt_node` s'occupent du reste.

---

## 📊 Statut

| Métrique | Valeur |
|----------|--------|
| Dernière | `v0.3.3-Beta` |
| Paquets | 4 (`cs_core` `cs_input` `cs_output` `cs_interfaces`) |
| Outils | 5 (`shell_exec` `file_rdwt` `message_send` `web_search` `web_chat`) |
| Capteurs | 3 (`system_status` `message_receive` `ros_msg`) |
| Nœuds | 10+ |

---

<div align="center">

**Construit avec ❤️ sur ROS2 Humble**

*"L'âme est dans le cloud, le corps est partout."*

</div>