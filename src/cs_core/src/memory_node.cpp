// Copyright (c) leaf
// SPDX-License-Identifier: MIT
//
// memory_node.cpp – 记忆管理节点
// 提供两个服务：
//   /<agent_name>/memory_recall   无输入，返回错误码与合入规则后的文本
//   /<agent_name>/memory_archive  输入 JSON 路径，压缩后存入日记并推送
//
// 依赖：libgit2（需启用 SSH 支持）、nlohmann/json、OpenAIClient

#include <rclcpp/rclcpp.hpp>
#include <git2.h>
#include <git2/errors.h>
#include <nlohmann/json.hpp>

#include "cs_interfaces/srv/memory_recall.hpp"
#include "cs_interfaces/constants.hpp"
#include "cs_interfaces/srv/memory_archive.hpp"
#include "cs_core/call_openai.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace fs = std::filesystem;

// =============================================================================
// SSH 凭证回调
// 优先级：SSH agent → ~/.ssh/id_rsa (无密码)
// =============================================================================
static int ssh_cred_callback(git_cred **out,
                             const char *url,
                             const char *username_from_url,
                             unsigned int allowed_types,
                             void * /*payload*/) {
    if (!(allowed_types & GIT_CREDTYPE_SSH_KEY))
        return 1;

    const char *username = username_from_url ? username_from_url : "git";

    // 1. 尝试 SSH agent（仅当 SSH_AUTH_SOCK 环境变量存在时）
    if (std::getenv("SSH_AUTH_SOCK")) {
        if (git_cred_ssh_key_from_agent(out, username) == 0)
            return 0;
    }

    // 2. 遍历常见私钥（无密码）
    const char *home = std::getenv("HOME");
    if (home) {
        const std::string keys[] = {
            "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
            "id_ed25519_github", "id_rsa_github", "id_ecdsa_github",
            "identity"   // 兼容旧格式
        };
        for (const auto &key : keys) {
            std::string priv_path = std::string(home) + "/.ssh/" + key;
            if (access(priv_path.c_str(), R_OK) != 0) continue;

            std::string pub_path = priv_path + ".pub";
            const char *pub = (access(pub_path.c_str(), R_OK) == 0)
                                ? pub_path.c_str() : nullptr;

            if (git_cred_ssh_key_new(out, username, pub,
                                     priv_path.c_str(), nullptr) == 0)
                return 0;
        }
    }

    return 1;
}

// -----------------------------------------------------------------------------
// MemoryNode 类定义
// -----------------------------------------------------------------------------
class MemoryNode : public rclcpp::Node {
public:
    MemoryNode();

private:
    // ---------- 参数 ----------
    std::string agent_name_;
    std::string repo_url_;    // SSH 格式，如 git@github.com:user/repo.git
    std::string repo_name_;   // 远端名称，默认 "origin"
    std::string repo_fork_;   // 分支名，默认 "main"
    std::string repo_dir_;
    std::string rule_path_;   // 默认 "prompts/RULE.md"
    std::string openai_base_url_;
    std::string openai_api_key_;
    std::string openai_model_;

    // ---------- 服务 ----------
    rclcpp::Service<cs_interfaces::srv::MemoryRecall>::SharedPtr recall_srv_;
    rclcpp::Service<cs_interfaces::srv::MemoryArchive>::SharedPtr archive_srv_;

    // ---------- 核心操作 ----------
    int ensure_repo_updated();
    std::string expand_text(const std::string &text,
                            const fs::path &repo_path,
                            int depth = 0) const;
    std::string utc_date_str() const;

    // ---------- 回调 ----------
    void on_recall(const std::shared_ptr<cs_interfaces::srv::MemoryRecall::Request> req,
                   std::shared_ptr<cs_interfaces::srv::MemoryRecall::Response> res);
    void on_archive(const std::shared_ptr<cs_interfaces::srv::MemoryArchive::Request> req,
                    std::shared_ptr<cs_interfaces::srv::MemoryArchive::Response> res);
};

// =============================================================================
// 构造函数：读取参数，创建服务
// =============================================================================
MemoryNode::MemoryNode()
    : Node("memory_node") {
    agent_name_ = declare_parameter<std::string>("agent_name", "");
    repo_url_   = declare_parameter<std::string>("repo_url", "");
    repo_name_  = declare_parameter<std::string>("repo_name", "origin");
    repo_fork_  = declare_parameter<std::string>("repo_fork", "main");
    repo_dir_   = declare_parameter<std::string>("repo_dir", "");
    rule_path_  = declare_parameter<std::string>("rule_path", "prompts/RULE.md");

    openai_base_url_ = declare_parameter<std::string>("openai_base_url", "https://api.deepseek.com");
    openai_api_key_  = declare_parameter<std::string>("openai_api_key", "");
    openai_model_    = declare_parameter<std::string>("openai_model", "deepseek-chat");

    if (openai_api_key_.empty()) {
        const char *env = std::getenv("OPENAI_API_KEY");
        if (env) openai_api_key_ = env;
    }

    if (agent_name_.empty() || repo_url_.empty() || repo_dir_.empty()) {
        RCLCPP_ERROR(get_logger(), "agent_name, repo_url, repo_dir 必须设置");
        rclcpp::shutdown();
        return;
    }

    std::string recall_srv_name  = "/" + agent_name_ + "/memory_recall";
    std::string archive_srv_name = "/" + agent_name_ + "/memory_archive";

    recall_srv_ = create_service<cs_interfaces::srv::MemoryRecall>(
        recall_srv_name,
        std::bind(&MemoryNode::on_recall, this, std::placeholders::_1, std::placeholders::_2));
    archive_srv_ = create_service<cs_interfaces::srv::MemoryArchive>(
        archive_srv_name,
        std::bind(&MemoryNode::on_archive, this, std::placeholders::_1, std::placeholders::_2));

    RCLCPP_INFO(get_logger(), "memory 节点启动，服务 %s / %s 已就绪",
                recall_srv_name.c_str(), archive_srv_name.c_str());
}

// =============================================================================
// 同步仓库：clone / fetch + hard reset 到远端分支
// 返回 0 成功（或网络不可达时跳过），-1 失败
// =============================================================================
int MemoryNode::ensure_repo_updated() {
    git_repository *repo = nullptr;

    if (!fs::exists(repo_dir_))
        fs::create_directories(repo_dir_);

    bool is_git = fs::exists(fs::path(repo_dir_) / ".git");

    if (!is_git) {
        // ---- clone ----
        git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
        clone_opts.fetch_opts.callbacks.credentials = ssh_cred_callback;

        RCLCPP_INFO(get_logger(), "克隆仓库 %s 到 %s", repo_url_.c_str(), repo_dir_.c_str());
        int err = git_clone(&repo, repo_url_.c_str(), repo_dir_.c_str(), &clone_opts);
        if (err != 0) {
            const git_error *e = git_error_last();
            if (e && e->klass == GIT_ERROR_NET) {
                RCLCPP_WARN(get_logger(), "克隆失败（网络不可达），跳过本次同步：%s",
                            e->message);
                return 0;
            }
            RCLCPP_ERROR(get_logger(), "克隆仓库失败：%s", e ? e->message : "未知错误");
            return -1;
        }
        git_repository_free(repo);
        return 0;
    }

    // ---- open & fetch ----
    if (git_repository_open(&repo, repo_dir_.c_str()) != 0) {
        const git_error *e = git_error_last();
        RCLCPP_ERROR(get_logger(), "打开仓库失败：%s", e ? e->message : "?");
        return -1;
    }

    git_remote *remote = nullptr;
    int err = git_remote_lookup(&remote, repo, repo_name_.c_str());
    if (err != 0) {
        err = git_remote_create(&remote, repo, repo_name_.c_str(), repo_url_.c_str());
        if (err != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "创建远端失败：%s", e ? e->message : "?");
            git_repository_free(repo);
            return -1;
        }
    } else {
        git_remote_set_url(repo, repo_name_.c_str(), repo_url_.c_str());
    }

    git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
    fetch_opts.callbacks.credentials = ssh_cred_callback;

    err = git_remote_fetch(remote, nullptr, &fetch_opts, nullptr);
    if (err != 0) {
        const git_error *e = git_error_last();
        if (e && e->klass == GIT_ERROR_NET) {
            RCLCPP_WARN(get_logger(), "fetch 失败（网络不可达），跳过本次同步：%s", e->message);
            git_remote_free(remote);
            git_repository_free(repo);
            return 0;
        }
        RCLCPP_ERROR(get_logger(), "fetch 失败：%s", e ? e->message : "?");
        git_remote_free(remote);
        git_repository_free(repo);
        return -1;
    }

    std::string remote_ref = cloud_soul::REFS_REMOTES_PREFIX + repo_name_ + "/" + repo_fork_;
    git_oid target_oid;
    if (git_reference_name_to_id(&target_oid, repo, remote_ref.c_str()) != 0) {
        RCLCPP_ERROR(get_logger(), "找不到远程分支 %s", remote_ref.c_str());
        git_remote_free(remote);
        git_repository_free(repo);
        return -1;
    }

    git_object *target_obj = nullptr;
    if (git_object_lookup(&target_obj, repo, &target_oid, GIT_OBJECT_COMMIT) != 0) {
        const git_error *e = git_error_last();
        RCLCPP_ERROR(get_logger(), "查找远程 commit 失败：%s", e ? e->message : "?");
        git_remote_free(remote);
        git_repository_free(repo);
        return -1;
    }

    err = git_reset(repo, target_obj, GIT_RESET_HARD, nullptr);
    git_object_free(target_obj);
    git_remote_free(remote);
    if (err != 0) {
        const git_error *e = git_error_last();
        RCLCPP_ERROR(get_logger(), "reset 失败：%s", e ? e->message : "?");
        git_repository_free(repo);
        return -1;
    }

    git_repository_free(repo);
    RCLCPP_INFO(get_logger(), "仓库已同步到分支 %s 的最新提交", repo_fork_.c_str());
    return 0;
}

// =============================================================================
// 递归替换 [path] 占位符
// =============================================================================
std::string MemoryNode::expand_text(const std::string &text,
                                    const fs::path &repo_path,
                                    int depth) const {
    if (depth > cloud_soul::FILE_INCLUDE_MAX_DEPTH) return text;
    std::regex bracket_re(R"(\[([^\[\]]+)\])");
    std::smatch m;
    std::string work = text;

    while (std::regex_search(work, m, bracket_re)) {
        std::string placeholder = m[0];
        std::string file_rel    = m[1];
        fs::path file_path = repo_path / file_rel;
        std::string replacement = placeholder;

        if (fs::exists(file_path)) {
            std::ifstream ifs(file_path);
            if (ifs) {
                std::stringstream ss;
                ss << ifs.rdbuf();
                replacement = expand_text(ss.str(), repo_path, depth + 1);
            }
        }
        work.replace(m.position(), m.length(), replacement);
    }
    return work;
}

// =============================================================================
// 获取 UTC 日期字符串 YYYYMMDD
// =============================================================================
std::string MemoryNode::utc_date_str() const {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm *gmt = std::gmtime(&tt);
    char buf[cloud_soul::DATE_BUF_SIZE];
    std::strftime(buf, sizeof(buf), "%Y%m%d", gmt);
    return std::string(buf);
}

// =============================================================================
// memory_recall 服务回调
// =============================================================================
void MemoryNode::on_recall(
    const std::shared_ptr<cs_interfaces::srv::MemoryRecall::Request> /*req*/,
    std::shared_ptr<cs_interfaces::srv::MemoryRecall::Response> res) {

    RCLCPP_INFO(get_logger(), "memory_recall 被调用");
    if (ensure_repo_updated() != 0) {
        res->error_code = cloud_soul::Err::MemArchive::FILE_NOT_FOUND;
        res->text = cloud_soul::Msg::REPO_SYNC_FAILED;
        return;
    }

    fs::path rule_file = fs::path(repo_dir_) / rule_path_;
    std::string raw;
    {
        std::ifstream ifs(rule_file);
        if (!ifs) {
            res->error_code = cloud_soul::Err::MemArchive::RENAME_FAILED;
            res->text = cloud_soul::Msg::RULE_OPEN_FAILED + rule_path_;
            RCLCPP_ERROR(get_logger(), "打开 %s 失败", rule_file.c_str());
            return;
        }
        std::stringstream ss;
        ss << ifs.rdbuf();
        raw = ss.str();
    }

    std::string expanded = expand_text(raw, fs::path(repo_dir_));
    res->error_code = 0;
    res->text = expanded;
    RCLCPP_INFO(get_logger(), "memory_recall 返回 %zu 字节文本", expanded.size());
}

// =============================================================================
// memory_archive 服务回调（修正文件流转逻辑）
// =============================================================================
void MemoryNode::on_archive(
    const std::shared_ptr<cs_interfaces::srv::MemoryArchive::Request> req,
    std::shared_ptr<cs_interfaces::srv::MemoryArchive::Response> res) {

    std::string json_path = req->json_path;
    RCLCPP_INFO(get_logger(), "memory_archive 被调用，文件：%s", json_path.c_str());

    if (!fs::exists(json_path)) {
        RCLCPP_ERROR(get_logger(), "JSON 文件不存在");
        res->error_code = cloud_soul::Err::MemArchive::FILE_NOT_FOUND;
        return;
    }

    // 处理中文件：json_path + ".processing"
    std::string processing_path = json_path + ".processing";
    std::error_code ec;

    // 原子重命名为 .processing
    fs::rename(json_path, processing_path, ec);
    if (ec) {
        RCLCPP_ERROR(get_logger(), "重命名失败：%s", ec.message().c_str());
        res->error_code = cloud_soul::Err::MemArchive::RENAME_FAILED;
        return;
    }

    // 辅助 lambda：失败时将 .processing 恢复为原始文件
    auto revert_processing = [&]() {
        std::error_code err;
        fs::rename(processing_path, json_path, err);
        if (err) {
            RCLCPP_ERROR(get_logger(), "恢复处理文件失败：%s", err.message().c_str());
        }
    };

    const int MAX_RETRIES = cloud_soul::ARCHIVE_PUSH_MAX_RETRIES;
    int attempt = 0;
    bool success = false;
    while (attempt < MAX_RETRIES && !success) {
        ++attempt;

        if (ensure_repo_updated() != 0) {
            RCLCPP_ERROR(get_logger(), "第 %d 次尝试：仓库同步失败", attempt);
            continue;
        }

        fs::path compress_file = fs::path(repo_dir_) / "prompts" / "COMPRESS.md";
        std::string sys_prompt;
        {
            std::ifstream ifs(compress_file);
            if (!ifs) {
                RCLCPP_ERROR(get_logger(), "无法打开 COMPRESS.md");
                revert_processing();
                res->error_code = cloud_soul::Err::MemArchive::COMPRESS_OPEN_FAILED;
                return;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            sys_prompt = ss.str();
        }

        std::string user_content;
        {
            std::ifstream ifs(processing_path);
            if (!ifs) {
                RCLCPP_ERROR(get_logger(), "无法打开 .processing 文件");
                revert_processing();
                res->error_code = cloud_soul::Err::MemArchive::PROCESSING_OPEN_FAILED;
                return;
            }
            std::stringstream ss;
            ss << ifs.rdbuf();
            user_content = ss.str();
        }

        openai_client::OpenAIClient client(openai_base_url_, openai_api_key_, openai_model_);
        client.add_message({{"role", "system"}, {"content", sys_prompt}});
        client.add_message({{"role", "user"}, {"content", user_content}});
        nlohmann::json reply = client.call_api(false);
        RCLCPP_DEBUG(get_logger(), "API 原始响应: %s", reply.dump().c_str());
        std::string assistant_text;
        bool api_ok = false;
        if (reply.contains("choices") && reply["choices"].is_array() && !reply["choices"].empty()) {
            auto &msg = reply["choices"][0]["message"];
            if (msg.contains("content")) {
                assistant_text = msg["content"].get<std::string>();
                api_ok = true;
            }
        } else if (reply.contains("content") && reply.contains("role")) {
            assistant_text = reply["content"].get<std::string>();
            api_ok = true;
        }
        if (!api_ok) {
            RCLCPP_ERROR(get_logger(), cloud_soul::Msg::LLM_FAILED);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::LLM_FAILED;
            return;
        }

        std::string date_str = utc_date_str();
        fs::path diaries_dir = fs::path(repo_dir_) / "diaries";
        fs::create_directories(diaries_dir, ec);
        if (ec) {
            RCLCPP_ERROR(get_logger(), "创建 diaries 目录失败");
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::DIARY_DIR_FAILED;
            return;
        }
        fs::path diary_file = diaries_dir / (date_str + ".md");
        {
            std::ofstream ofs(diary_file, std::ios::app);
            if (!ofs) {
                RCLCPP_ERROR(get_logger(), cloud_soul::Msg::DIARY_OPEN_FAILED);
                revert_processing();
                res->error_code = cloud_soul::Err::MemArchive::DIARY_OPEN_FAILED;
                return;
            }
            ofs << assistant_text << "\n";
        }

        // ---- git add / commit / push ----
        git_repository *repo = nullptr;
        if (git_repository_open(&repo, repo_dir_.c_str()) != 0) {
            RCLCPP_ERROR(get_logger(), cloud_soul::Msg::REPO_OPEN_FAILED);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_OPEN_FAILED;
            return;
        }

        git_index *index = nullptr;
        if (git_repository_index(&index, repo) != 0) {
            RCLCPP_ERROR(get_logger(), "获取 index 失败");
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_INDEX_FAILED;
            return;
        }

        std::string rel_path = std::string(cloud_soul::DIARIES_REL_DIR) + "/" + date_str + ".md";
        if (git_index_add_bypath(index, rel_path.c_str()) != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "git add 失败：%s", e ? e->message : "?");
            git_index_free(index);
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_ADD_FAILED;
            return;
        }
        if (git_index_write(index) != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "git index write 失败：%s", e ? e->message : "?");
            git_index_free(index);
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_IDX_WRITE_FAILED;
            return;
        }

        git_oid tree_oid;
        if (git_index_write_tree(&tree_oid, index) != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "创建 tree 失败：%s", e ? e->message : "?");
            git_index_free(index);
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_TREE_WRITE_FAILED;
            return;
        }
        git_tree *tree = nullptr;
        if (git_tree_lookup(&tree, repo, &tree_oid) != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "查找 tree 对象失败：%s", e ? e->message : "?");
            git_index_free(index);
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_TREE_LOOKUP_FAILED;
            return;
        }
        git_index_free(index);

        git_oid parent_oid;
        bool have_parent = false;
        git_reference *head_ref = nullptr;
        if (git_repository_head(&head_ref, repo) == 0) {
            const git_oid *target = git_reference_target(head_ref);
            if (target) {
                git_oid_cpy(&parent_oid, target);
                have_parent = true;
            }
            git_reference_free(head_ref);
        }

        git_signature *sig = nullptr;
        if (git_signature_default(&sig, repo) != 0)
            git_signature_now(&sig, "memory", "memory@localhost");

        std::string commit_msg = "archive diary " + date_str;
        git_oid commit_oid;
        int cmt_err = git_commit_create_v(
            &commit_oid, repo, "HEAD",
            sig, sig, nullptr, commit_msg.c_str(),
            tree, have_parent ? 1 : 0, have_parent ? &parent_oid : nullptr);
        git_signature_free(sig);
        git_tree_free(tree);
        if (cmt_err != 0) {
            const git_error *e = git_error_last();
            RCLCPP_ERROR(get_logger(), "git commit 失败：%s", e ? e->message : "?");
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_COMMIT_FAILED;
            return;
        }

        git_remote *remote = nullptr;
        if (git_remote_lookup(&remote, repo, repo_name_.c_str()) != 0) {
            RCLCPP_ERROR(get_logger(), "查找远端 %s 失败", repo_name_.c_str());
            git_repository_free(repo);
            revert_processing();
            res->error_code = cloud_soul::Err::MemArchive::GIT_REMOTE_FAILED;
            return;
        }

        std::string push_refspec = cloud_soul::REFS_HEADS_PREFIX + repo_fork_ + ":refs/heads/" + repo_fork_;
        git_push_options push_opts = GIT_PUSH_OPTIONS_INIT;
        push_opts.callbacks.credentials = ssh_cred_callback;

        char *refspecs[1] = { const_cast<char *>(push_refspec.c_str()) };
        git_strarray ref_array = { refspecs, 1 };
        int push_err = git_remote_push(remote, &ref_array, &push_opts);

        if (push_err == 0) {
            RCLCPP_INFO(get_logger(), cloud_soul::Msg::PUSH_SUCCESS);
            success = true;
        } else {
            const git_error *e = git_error_last();
            std::string err_msg = e ? e->message : "";
            RCLCPP_WARN(get_logger(), "推送失败：%s", err_msg.c_str());

            bool is_conflict = (err_msg.find("non-fast-forward") != std::string::npos ||
                                err_msg.find("cannot push non-fastforward") != std::string::npos ||
                                push_err == GIT_ENONFASTFORWARD);
            if (is_conflict && attempt < MAX_RETRIES) {
                RCLCPP_INFO(get_logger(), "检测到远程更新，重新拉取并重试...");
                // 重新拉取后重试，但保留 .processing 文件不变
            } else {
                RCLCPP_ERROR(get_logger(), cloud_soul::Msg::PUSH_FAILED_NO_RETRY);
                git_remote_free(remote);
                git_repository_free(repo);
                revert_processing();
                res->error_code = cloud_soul::Err::MemArchive::GIT_PUSH_FAILED;
                return;
            }
        }
        git_remote_free(remote);
        git_repository_free(repo);
    }

    if (!success) {
        // 如果重试耗尽仍未成功，恢复文件
        revert_processing();
        res->error_code = cloud_soul::Err::MemArchive::PUSH_RETRY_EXHAUSTED;
        return;
    }

    // 成功：将 .processing 重命名为 .json.done
    std::string done_path = json_path + ".done";
    fs::rename(processing_path, done_path, ec);
    if (ec) {
        RCLCPP_ERROR(get_logger(), "重命名为 .done 失败：%s", ec.message().c_str());
        // 此时 .processing 仍存在，尝试恢复为原始文件
        revert_processing();
        res->error_code = cloud_soul::Err::MemArchive::DONE_RENAME_FAILED;
        return;
    }

    res->error_code = 0;
    RCLCPP_INFO(get_logger(), "memory_archive 完成，文件标记为 %s", done_path.c_str());
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char **argv) {
    git_libgit2_init();
    rclcpp::init(argc, argv);
    auto node = std::make_shared<MemoryNode>();
    rclcpp::spin(node);
    rclcpp::shutdown();
    git_libgit2_shutdown();
    return 0;
}