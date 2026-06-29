#include "config_page.h"

#include "config.h"
#include "util.h"
#include "win_util.h"

#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

namespace ncw
{
namespace
{
std::string HtmlEscape(const std::string &text)
{
    std::string output;
    output.reserve(text.size());
    for (char ch : text)
    {
        switch (ch)
        {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}

std::string PathValue(const std::filesystem::path &path)
{
    return path.empty() ? "" : WideToUtf8(path.wstring());
}

void AddInput(std::ostringstream *html, const std::string &key, const std::string &label, const std::string &value,
              const std::string &note = "", const std::string &type = "text")
{
    *html << "<label><span>" << HtmlEscape(label) << "</span><input data-key=\"" << HtmlEscape(key) << "\" type=\""
          << HtmlEscape(type) << "\" value=\"" << HtmlEscape(value) << "\">";
    if (!note.empty())
    {
        *html << "<small>" << HtmlEscape(note) << "</small>";
    }
    *html << "</label>\n";
}

void AddCheckbox(std::ostringstream *html, const std::string &key, const std::string &label, bool checked,
                 const std::string &note = "")
{
    *html << "<label class=\"check\"><input data-key=\"" << HtmlEscape(key) << "\" type=\"checkbox\""
          << (checked ? " checked" : "") << "><span>" << HtmlEscape(label) << "</span>";
    if (!note.empty())
    {
        *html << "<small>" << HtmlEscape(note) << "</small>";
    }
    *html << "</label>\n";
}

void AddSectionStart(std::ostringstream *html, const std::string &title, const std::string &desc)
{
    *html << "<section><h2>" << HtmlEscape(title) << "</h2><p>" << HtmlEscape(desc) << "</p><div class=\"grid\">\n";
}

void AddSectionEnd(std::ostringstream *html)
{
    *html << "</div></section>\n";
}
}

std::filesystem::path WriteConfigPage(const AppConfig &config, const std::filesystem::path &config_path)
{
    const std::filesystem::path output_path = config.state_dir / L"notion-clipboard-config.html";
    std::filesystem::create_directories(output_path.parent_path());

    std::ostringstream html;
    html << R"(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Notion Clipboard Win 配置</title>
<style>
:root{color-scheme:light dark;--bg:#f6f7f9;--panel:#fff;--text:#172033;--muted:#667085;--line:#d9dee8;--accent:#1f6feb;--accent2:#0f766e}
@media (prefers-color-scheme:dark){:root{--bg:#111827;--panel:#182233;--text:#edf2f7;--muted:#9aa8bd;--line:#324055;--accent:#5aa2ff;--accent2:#2dd4bf}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 "Segoe UI",system-ui,sans-serif}
header{position:sticky;top:0;z-index:2;background:color-mix(in srgb,var(--panel) 92%,transparent);border-bottom:1px solid var(--line);backdrop-filter:blur(8px)}
.bar{max-width:1180px;margin:auto;padding:16px 20px;display:flex;gap:12px;align-items:center;justify-content:space-between}
h1{font-size:20px;margin:0}.path{color:var(--muted);font-size:12px;word-break:break-all}.wrap{max-width:1180px;margin:0 auto;padding:20px;display:grid;grid-template-columns:minmax(0,1.1fr) minmax(360px,.9fr);gap:18px}
section,.output{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:16px;margin-bottom:16px}h2{font-size:15px;margin:0 0 4px}p{margin:0 0 12px;color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}label{display:grid;gap:5px}label span{font-weight:600}input,select,textarea{width:100%;border:1px solid var(--line);background:var(--bg);color:var(--text);border-radius:6px;padding:9px 10px;font:inherit}
small{color:var(--muted)}.check{grid-template-columns:auto 1fr;align-items:start}.check input{width:auto;margin-top:3px}.check small{grid-column:2}
.actions{display:flex;gap:8px;flex-wrap:wrap}button,.download{border:0;border-radius:6px;background:var(--accent);color:white;padding:9px 12px;font-weight:600;cursor:pointer;text-decoration:none}.secondary{background:var(--accent2)}
textarea{min-height:520px;resize:vertical;font-family:Consolas,monospace;font-size:12px}.target{display:flex;gap:10px;align-items:center}.target select{max-width:280px}.hint{border-left:3px solid var(--accent);padding-left:10px;color:var(--muted)}
@media (max-width:900px){.wrap{grid-template-columns:1fr}.grid{grid-template-columns:1fr}.bar{align-items:flex-start;flex-direction:column}}
</style>
</head>
<body>
<header><div class="bar"><div><h1>Notion Clipboard Win 配置</h1><div class="path">配置文件：)"
         << HtmlEscape(WideToUtf8(config_path.wstring())) << R"(</div></div><div class="target"><strong>上传后端</strong><select data-key="upload_target" id="target">
<option value="notion">Notion</option><option value="markdown_file">Markdown 文件</option><option value="obsidian">Obsidian</option><option value="local_git">本地 Git</option><option value="webhook">Webhook</option><option value="github_gist">GitHub Gist</option><option value="github_repo">GitHub 仓库</option><option value="yuque">语雀</option><option value="feishu_doc">飞书文档</option>
</select></div></div></header>
<main class="wrap"><div>
)";

    AddSectionStart(&html, "Notion", "上传为 Notion 数据源里的页面。");
    AddInput(&html, "notion_token", "Notion Token", config.notion_token, "secret_xxx", "password");
    AddInput(&html, "data_source_id", "Data Source ID", config.data_source_id);
    AddInput(&html, "database_id", "Database ID", config.database_id, "仅保留旧配置兼容。");
    AddInput(&html, "title_property_name", "标题属性", config.title_property_name);
    AddInput(&html, "content_property_name", "内容预览属性", config.content_property_name);
    AddInput(&html, "created_time_property_name", "创建时间属性", config.created_time_property_name);
    AddInput(&html, "content_property_max_chars", "内容预览最大字符", std::to_string(config.content_property_max_chars),
             "", "number");
    AddSectionEnd(&html);

    AddSectionStart(&html, "本地文件", "写入 Markdown 文件、Obsidian vault 或本地 Git 工作区。");
    AddInput(&html, "markdown_output_dir", "Markdown 输出目录", PathValue(config.markdown_output_dir));
    AddInput(&html, "obsidian_vault_dir", "Obsidian Vault", PathValue(config.obsidian_vault_dir));
    AddInput(&html, "obsidian_folder", "Obsidian 子目录", config.obsidian_folder);
    AddInput(&html, "obsidian_filename_prefix", "Obsidian 文件名前缀", config.obsidian_filename_prefix);
    AddInput(&html, "local_git_repo_dir", "本地 Git 仓库目录", PathValue(config.local_git_repo_dir));
    AddInput(&html, "local_git_directory", "Git 子目录", config.local_git_directory);
    AddInput(&html, "local_git_filename_prefix", "Git 文件名前缀", config.local_git_filename_prefix);
    AddCheckbox(&html, "local_git_auto_commit", "写入后自动 git add/commit", config.local_git_auto_commit,
                "需要本机 git 可用，且仓库已配置 user.name/user.email。");
    AddSectionEnd(&html);

    AddSectionStart(&html, "HTTP 与 GitHub", "Webhook、Gist 和 GitHub 仓库目标。");
    AddInput(&html, "webhook_url", "Webhook URL", config.webhook_url);
    AddInput(&html, "webhook_bearer_token", "Webhook Bearer Token", config.webhook_bearer_token, "", "password");
    AddInput(&html, "github_token", "GitHub Token", config.github_token, "Gist 或 Contents 写权限。", "password");
    AddCheckbox(&html, "github_gist_public", "GitHub Gist 公开", config.github_gist_public);
    AddInput(&html, "github_gist_filename_prefix", "Gist 文件名前缀", config.github_gist_filename_prefix);
    AddInput(&html, "github_repo_owner", "GitHub 仓库 owner", config.github_repo_owner);
    AddInput(&html, "github_repo_name", "GitHub 仓库名", config.github_repo_name);
    AddInput(&html, "github_repo_branch", "GitHub 分支", config.github_repo_branch, "留空使用默认分支。");
    AddInput(&html, "github_repo_directory", "GitHub 仓库目录", config.github_repo_directory);
    AddInput(&html, "github_repo_filename_prefix", "GitHub 文件名前缀", config.github_repo_filename_prefix);
    AddSectionEnd(&html);

    AddSectionStart(&html, "语雀", "通过语雀 Open API v2 创建 Markdown 文档。");
    AddInput(&html, "yuque_token", "语雀 Token", config.yuque_token, "", "password");
    AddInput(&html, "yuque_namespace", "语雀知识库 namespace", config.yuque_namespace, "通常形如 login/repo-slug。");
    AddInput(&html, "yuque_slug_prefix", "语雀 slug 前缀", config.yuque_slug_prefix);
    AddSectionEnd(&html);

    AddSectionStart(&html, "飞书文档", "创建飞书文档，并写入 Markdown 文本块。");
    AddInput(&html, "feishu_app_id", "飞书 App ID", config.feishu_app_id);
    AddInput(&html, "feishu_app_secret", "飞书 App Secret", config.feishu_app_secret, "", "password");
    AddInput(&html, "feishu_folder_token", "飞书 Folder Token", config.feishu_folder_token, "留空使用应用默认位置。");
    AddSectionEnd(&html);

    AddSectionStart(&html, "应用行为", "热键、自动监听和重试参数。");
    AddInput(&html, "state_dir", "状态目录", PathValue(config.state_dir));
    AddInput(&html, "hotkey", "全局热键", config.hotkey);
    AddCheckbox(&html, "enable_hotkey", "启用全局热键", config.enable_hotkey);
    AddCheckbox(&html, "enable_clipboard_listener", "自动监听剪贴板", config.enable_clipboard_listener);
    AddCheckbox(&html, "tray_notifications", "托盘通知", config.tray_notifications);
    AddCheckbox(&html, "start_with_windows", "开机自动启动", config.start_with_windows);
    AddCheckbox(&html, "upload_initial_clipboard", "启动后上传当前剪贴板", config.upload_initial_clipboard);
    AddInput(&html, "debounce_ms", "剪贴板 debounce ms", std::to_string(config.debounce_ms), "", "number");
    AddInput(&html, "duplicate_suppression_ms", "重复抑制 ms", std::to_string(config.duplicate_suppression_ms), "",
             "number");
    AddInput(&html, "max_clipboard_bytes", "最大剪贴板字节数", std::to_string(config.max_clipboard_bytes), "",
             "number");
    AddInput(&html, "min_request_interval_ms", "HTTP 最小间隔 ms", std::to_string(config.min_request_interval_ms), "",
             "number");
    AddInput(&html, "append_batch_size", "Notion 每批 block 数", std::to_string(config.append_batch_size), "",
             "number");
    AddInput(&html, "max_retry_attempts", "持久队列重试次数", std::to_string(config.max_retry_attempts), "",
             "number");
    AddInput(&html, "http_retry_attempts", "HTTP 短重试次数", std::to_string(config.http_retry_attempts), "",
             "number");
    AddSectionEnd(&html);

    html << R"(
</div><aside class="output"><h2>输出 ini</h2><p class="hint">页面会输出包含 token 的完整配置。不要把生成内容提交到仓库。</p><div class="actions"><button id="copy">复制配置</button><a id="download" class="download secondary" download="notion_clipboard_win.ini">下载 ini</a><button id="reveal" class="secondary">显示/隐藏 token</button></div><textarea id="ini" spellcheck="false"></textarea></aside></main>
<script>
const order=["upload_target","notion_token","data_source_id","database_id","title_property_name","content_property_name","content_property_max_chars","created_time_property_name","markdown_output_dir","obsidian_vault_dir","obsidian_folder","obsidian_filename_prefix","local_git_repo_dir","local_git_directory","local_git_filename_prefix","local_git_auto_commit","webhook_url","webhook_bearer_token","github_token","github_gist_public","github_gist_filename_prefix","github_repo_owner","github_repo_name","github_repo_branch","github_repo_directory","github_repo_filename_prefix","yuque_token","yuque_namespace","yuque_slug_prefix","feishu_app_id","feishu_app_secret","feishu_folder_token","state_dir","hotkey","enable_hotkey","enable_clipboard_listener","tray_notifications","start_with_windows","upload_initial_clipboard","debounce_ms","duplicate_suppression_ms","max_clipboard_bytes","min_request_interval_ms","append_batch_size","max_retry_attempts","http_retry_attempts"];
const target=document.getElementById("target"); target.value=)"
         << '"' << HtmlEscape(config.upload_target) << '"' << R"(;
function val(key){const el=document.querySelector(`[data-key="${key}"]`); if(!el)return ""; return el.type==="checkbox"?(el.checked?"true":"false"):el.value.trim();}
function build(){const text=order.map(k=>`${k}=${val(k)}`).join("\n")+"\n"; document.getElementById("ini").value=text; document.getElementById("download").href=URL.createObjectURL(new Blob([text],{type:"text/plain;charset=utf-8"}));}
document.querySelectorAll("[data-key]").forEach(el=>el.addEventListener("input",build));
document.getElementById("copy").addEventListener("click",async()=>{build(); await navigator.clipboard.writeText(document.getElementById("ini").value);});
document.getElementById("reveal").addEventListener("click",()=>document.querySelectorAll('input[type="password"],input[data-was-password]').forEach(el=>{if(el.type==="password"){el.dataset.wasPassword="1";el.type="text"}else{el.type="password"}}));
build();
</script>
</body></html>
)";

    AtomicWriteFile(output_path, html.str());
    return output_path;
}

int RunConfigPageSelfTest()
{
    bool ok = true;
    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] config page self-test: " << message << "\n";
        ok = false;
    };

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (L"notion-clipboard-win-config-page-test-" +
                                                  std::to_wstring(NowUnixMs()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    try
    {
        AppConfig config;
        config.state_dir = root / L"state";
        config.upload_target = "yuque";
        config.notion_token = "notion_secret";
        config.github_token = "github_secret";
        config.yuque_token = "yuque_secret";
        config.feishu_app_secret = "feishu_secret";
        config.obsidian_vault_dir = root / L"vault";
        config.local_git_repo_dir = root / L"repo";
        config.yuque_namespace = "team/book";

        const std::filesystem::path page = WriteConfigPage(config, root / L"notion_clipboard_win.ini");
        if (!std::filesystem::exists(page))
        {
            fail("page file was not created");
        }
        else
        {
            const std::string html = ReadWholeFile(page);
            for (const char *needle : {"notion_secret", "github_secret", "yuque_secret", "option value=\"obsidian\"",
                                       "option value=\"local_git\"", "option value=\"yuque\"",
                                       "option value=\"feishu_doc\"", "yuque_namespace", "feishu_secret",
                                       "local_git_auto_commit"})
            {
                if (html.find(needle) == std::string::npos)
                {
                    fail(std::string("missing expected config page content: ") + needle);
                }
            }
        }
    }
    catch (const std::exception &ex)
    {
        fail(ex.what());
    }

    std::filesystem::remove_all(root, ignored);
    if (ok)
    {
        std::cout << "[PASS] local config page\n";
    }
    return ok ? 0 : 1;
}
}
