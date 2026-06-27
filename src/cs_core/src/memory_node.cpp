// Copyright (c) leaf
// SPDX-License-Identifier: MIT

#include <rclcpp/rclcpp.hpp>
#include <cs_interfaces/srv/memory_recall.hpp>
#include <cs_interfaces/srv/memory_archive.hpp>

#include <git2.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <regex>
#include <chrono>
#include <iomanip>
#include <thread>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "cs_core/call_openai.hpp"

using json = nlohmann::json;
using MemoryRecall  = cs_interfaces::srv::MemoryRecall;
using MemoryArchive = cs_interfaces::srv::MemoryArchive;

class MemoryNode : public rclcpp::Node {
public:
    MemoryNode() : Node("memory_node") {
        this->declare_parameter<std::string>("agent_name", "");
        this->declare_parameter<std::string>("repo_url", "");
        this->declare_parameter<std::string>("repo_dir", "");
        this->declare_parameter<std::string>("repo_name", "origin");
        this->declare_parameter<std::string>("repo_fork", "main");
        this->declare_parameter<std::string>("rule_path", "prompts/RULE.md");
        this->declare_parameter<std::string>("openai_base_url", "");
        this->declare_parameter<std::string>("openai_api_key", "");
        this->declare_parameter<std::string>("openai_model", "");
        this->declare_parameter<int>("pull_retry_max", 3);
        this->declare_parameter<int>("push_retry_max", 5);

        agent_name_ = get_param("agent_name");
        repo_url_   = get_param("repo_url");
        repo_dir_   = get_param("repo_dir");
        repo_name_  = get_param("repo_name");
        repo_fork_  = get_param("repo_fork");
        rule_path_  = get_param("rule_path");
        pull_retry_ = this->get_parameter("pull_retry_max").as_int();
        push_retry_ = this->get_parameter("push_retry_max").as_int();

        git_libgit2_init();
        openai_client_ = std::make_unique<openai_client::OpenAIClient>(
            get_param("openai_base_url"),
            get_param("openai_api_key"),
            get_param("openai_model"));

        recall_srv_ = this->create_service<MemoryRecall>(
            "/" + agent_name_ + "/memory_recall",
            std::bind(&MemoryNode::on_recall, this,
                      std::placeholders::_1, std::placeholders::_2));
        archive_srv_ = this->create_service<MemoryArchive>(
            "/" + agent_name_ + "/memory_archive",
            std::bind(&MemoryNode::on_archive, this,
                      std::placeholders::_1, std::placeholders::_2));
    }

    ~MemoryNode() { git_libgit2_shutdown(); }

private:
    // ---------- 参数辅助 ----------
    std::string get_param(const std::string& name) {
        return this->get_parameter(name).as_string();
    }

    // ---------- recall ----------
    void on_recall(const std::shared_ptr<MemoryRecall::Request>,
                   std::shared_ptr<MemoryRecall::Response> res) {
        if (!ensure_repo()) { res->error_code = -1; res->text = "repo sync failed"; return; }
        std::string content;
        if (!read_file(repo_dir_ + "/" + rule_path_, content)) {
            res->error_code = -1; res->text = "rule file not found"; return;
        }
        res->error_code = 0;
        res->text = expand_includes(content, repo_dir_);
    }

    // ---------- archive ----------
    void on_archive(const std::shared_ptr<MemoryArchive::Request> req,
                    std::shared_ptr<MemoryArchive::Response> res) {
        std::string src = req->json_path;
        std::string pro = src + ".processing";
        std::string done = src + ".done";

        if (std::rename(src.c_str(), pro.c_str()) != 0) {
            res->error_code = -1; return;
        }

        try {
            if (!ensure_repo()) { revert(pro, src); res->error_code = -1; return; }

            std::string json_str;
            if (!read_file(pro, json_str)) { revert(pro, src); res->error_code = -1; return; }

            std::string sys;
            read_file(repo_dir_ + "/prompts/COMPRESS.md", sys);
            if (sys.empty()) sys = "Compress the JSON into a short summary.";

            openai_client_->clear_messages();
            openai_client_->add_message({{"role","system"}, {"content",sys}});
            openai_client_->add_message({{"role","user"}, {"content",json_str}});
            json ai = openai_client_->call_api(false, nullptr);   // may throw
            if (!ai.contains("content") || !ai["content"].is_string()) {
                revert(pro, src); res->error_code = -1; return;
            }
            std::string summary = ai["content"];

            auto t = std::time(nullptr);
            auto tm = *std::localtime(&t);
            char date[9];
            std::strftime(date, sizeof(date), "%Y%m%d", &tm);
            std::string diary_dir = repo_dir_ + "/diaries";
            mkdir(diary_dir.c_str(), 0755);
            std::string diary_path = diary_dir + "/" + date + ".md";

            std::ofstream ofs(diary_path, std::ios::app);
            if (!ofs) { revert(pro, src); res->error_code = -1; return; }
            ofs << "\n## " << date << "\n\n" << summary << "\n";
            ofs.close();

            if (!commit_file(diary_path)) { revert(pro, src); res->error_code = -1; return; }

            // 推送，带中断检测
            bool pushed = false;
            for (int i = 0; (push_retry_ < 0 || i < push_retry_); ++i) {
                if (!rclcpp::ok()) break;   // SIGINT 中断立即退出
                if (git_push()) { pushed = true; break; }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!pushed) { revert(pro, src); res->error_code = -1; return; }

            if (std::rename(pro.c_str(), done.c_str()) != 0) {
                revert(pro, src);
                res->error_code = -1; return;
            }
            res->error_code = 0;
        } catch (...) {
            revert(pro, src);
            res->error_code = -1;
        }
    }

    // ---------- 仓库同步 ----------
    bool ensure_repo() {
        for (int i = 0; pull_retry_ < 0 || i < pull_retry_; ++i) {
            if (!rclcpp::ok()) return false;

            git_repository* repo = nullptr;
            bool exists = (git_repository_open_ext(&repo, repo_dir_.c_str(), 0, nullptr) == 0);
            if (repo) git_repository_free(repo);

            if (!exists) {
                git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
                opts.fetch_opts.callbacks.credentials = cred_cb;
                opts.checkout_branch = repo_fork_.c_str();
                if (::git_clone(&repo, repo_url_.c_str(), repo_dir_.c_str(), &opts) == 0) {
                    git_repository_free(repo);
                    return true;
                }
            } else {
                if (git_fetch_and_reset() == 0) return true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return false;
    }

    int git_fetch_and_reset() {
        git_repository* repo;
        if (git_repository_open(&repo, repo_dir_.c_str())) return -1;

        git_remote* remote;
        if (git_remote_lookup(&remote, repo, repo_name_.c_str())) {
            git_repository_free(repo); return -1;
        }

        git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
        opts.callbacks.credentials = cred_cb;
        if (git_remote_fetch(remote, nullptr, &opts, nullptr)) {
            git_remote_free(remote); git_repository_free(repo); return -1;
        }
        git_remote_free(remote);

        git_object* target;
        std::string ref = "refs/remotes/" + repo_name_ + "/" + repo_fork_;
        if (git_revparse_single(&target, repo, ref.c_str())) {
            git_repository_free(repo); return -1;
        }
        git_reset(repo, target, GIT_RESET_HARD, nullptr);
        git_object_free(target);

        std::string local = "refs/heads/" + repo_fork_;
        git_reference* branch = nullptr;
        if (git_reference_lookup(&branch, repo, local.c_str()) != 0) {
            git_reference_create(&branch, repo, local.c_str(), git_object_id(target), 1, nullptr);
        }
        if (branch) git_reference_free(branch);
        git_repository_free(repo);
        return 0;
    }

    // ---------- 提交 ----------
    bool commit_file(const std::string& file_path) {
        git_repository* repo;
        if (git_repository_open(&repo, repo_dir_.c_str())) return false;

        git_index* idx;
        if (git_repository_index(&idx, repo)) { git_repository_free(repo); return false; }
        git_index_add_bypath(idx, file_path.substr(repo_dir_.size()+1).c_str());
        git_index_write(idx);
        git_oid tree_id;
        git_index_write_tree(&tree_id, idx);
        git_index_free(idx);

        git_tree* tree;
        git_tree_lookup(&tree, repo, &tree_id);
        git_signature* sig;
        git_signature_default(&sig, repo);
        if (!sig) git_signature_now(&sig, "memory", "memory@node");

        const ::git_commit* parent = nullptr;
        int parent_count = 0;
        git_reference* head;
        if (git_repository_head(&head, repo) == 0) {
            git_reference_peel((git_object**)&parent, head, GIT_OBJECT_COMMIT);
            parent_count = 1;
            git_reference_free(head);
        }

        git_oid commit_id;
        bool ok = git_commit_create(&commit_id, repo, "HEAD", sig, sig, "UTF-8",
                                    "auto archive", tree, parent_count,
                                    parent_count ? &parent : nullptr) == 0;
        git_signature_free(sig);
        git_tree_free(tree);
        git_repository_free(repo);
        return ok;
    }

    // ---------- 推送 ----------
    bool git_push() {
        git_repository* repo;
        if (git_repository_open(&repo, repo_dir_.c_str())) return false;

        git_remote* remote;
        if (git_remote_lookup(&remote, repo, repo_name_.c_str())) {
            git_repository_free(repo); return false;
        }

        char refspec[256];
        snprintf(refspec, sizeof(refspec), "refs/heads/%s:refs/heads/%s",
                 repo_fork_.c_str(), repo_fork_.c_str());
        const char* refs[] = {refspec, nullptr};
        git_strarray arr = {(char**)refs, 1};
        git_push_options popts = GIT_PUSH_OPTIONS_INIT;
        popts.callbacks.credentials = cred_cb;
        int err = git_remote_push(remote, &arr, &popts);
        git_remote_free(remote);
        git_repository_free(repo);
        return err == 0;
    }

    // ---------- SSH 凭证 ----------
    static int cred_cb(git_cred** cred, const char*, const char* user, unsigned int, void*) {
        return git_cred_ssh_key_from_agent(cred, user ? user : "git");
    }

    // ---------- 文件工具 ----------
    static bool read_file(const std::string& path, std::string& out) {
        std::ifstream f(path);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
    }

    static void revert(const std::string& pro, const std::string& orig) {
        struct stat st;
        if (::stat(pro.c_str(), &st) != 0) return;
        if (::stat(orig.c_str(), &st) == 0) {
            std::remove(pro.c_str());
            return;
        }
        if (std::rename(pro.c_str(), orig.c_str()) != 0) {
            std::ifstream src(pro, std::ios::binary);
            std::ofstream dst(orig, std::ios::binary);
            if (src && dst) dst << src.rdbuf();
            std::remove(pro.c_str());
        }
    }

    // ---------- 占位符展开（防循环、防爆栈） ----------
    static std::string expand_includes(const std::string& text,
                                       const std::string& base_dir) {
        std::regex re(R"(\[([^\]]+)\])");
        std::string result;
        size_t last = 0;
        // 循环检测：记录当前正在展开的绝对路径栈
        static thread_local std::unordered_set<std::string> expanding;
        static const int MAX_DEPTH = 10;

        for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
             it != std::sregex_iterator(); ++it) {
            result += text.substr(last, it->position() - last);
            std::string path = (*it)[1].str();
            std::string full = base_dir + "/" + path;

            // 防止递归过深
            if (expanding.size() >= MAX_DEPTH) {
                result += it->str();
                last = it->position() + it->length();
                continue;
            }

            // 循环引用检测
            if (expanding.count(full)) {
                result += it->str();   // 保留原文，不展开
                last = it->position() + it->length();
                continue;
            }

            std::string content;
            if (read_file(full, content)) {
                expanding.insert(full);
                result += expand_includes(content, base_dir);
                expanding.erase(full);
            } else {
                result += it->str();
            }
            last = it->position() + it->length();
        }
        result += text.substr(last);
        return result;
    }

    std::string agent_name_, repo_url_, repo_dir_, repo_name_, repo_fork_, rule_path_;
    int pull_retry_, push_retry_;
    rclcpp::Service<MemoryRecall>::SharedPtr recall_srv_;
    rclcpp::Service<MemoryArchive>::SharedPtr archive_srv_;
    std::unique_ptr<openai_client::OpenAIClient> openai_client_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<MemoryNode>());
    rclcpp::shutdown();
}