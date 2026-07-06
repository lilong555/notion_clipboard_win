#include "config_page.h"

#include "config.h"
#include "obsidian.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ncw
{
namespace
{
constexpr const char *kCustomPickerValue = "__custom__";

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

std::string NormalizePathKey(const std::filesystem::path &input)
{
    if (input.empty())
    {
        return "";
    }

    std::error_code ec;
    std::filesystem::path path = input.is_absolute() ? input : std::filesystem::absolute(input, ec);
    if (ec)
    {
        path = input;
        ec.clear();
    }

    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        normalized = path.lexically_normal();
    }

    std::wstring wide = normalized.wstring();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    while (wide.size() > 3 && (wide.back() == L'\\' || wide.back() == L'/'))
    {
        wide.pop_back();
    }
    return ToLowerAscii(WideToUtf8(wide));
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

bool HasOptionValue(const std::vector<std::pair<std::string, std::string>> &options, const std::string &value)
{
    return std::any_of(options.begin(), options.end(), [&](const auto &option)
                       {
                           return option.first == value;
                       });
}

void AddSelectWithCustomInput(std::ostringstream *html, const std::string &key, const std::string &label,
                              const std::string &value,
                              const std::vector<std::pair<std::string, std::string>> &options,
                              const std::string &custom_label, const std::string &custom_placeholder,
                              const std::string &note = "")
{
    const bool known_value = HasOptionValue(options, value);
    *html << "<label><span>" << HtmlEscape(label) << "</span><select data-choice-target=\"" << HtmlEscape(key)
          << "\">";
    for (const auto &option : options)
    {
        *html << "<option value=\"" << HtmlEscape(option.first) << "\""
              << (known_value && option.first == value ? " selected" : "") << ">" << HtmlEscape(option.second)
              << "</option>";
    }
    *html << "<option value=\"" << kCustomPickerValue << "\"" << (known_value ? "" : " selected") << ">"
          << HtmlEscape(custom_label) << "</option></select>";
    *html << "<input data-key=\"" << HtmlEscape(key) << "\" type=\"hidden\" value=\"" << HtmlEscape(value) << "\">";
    *html << "<input data-custom-key=\"" << HtmlEscape(key) << "\" type=\"text\" value=\""
          << HtmlEscape(known_value ? "" : value) << "\" placeholder=\"" << HtmlEscape(custom_placeholder) << "\""
          << (known_value ? " hidden" : "") << ">";
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

void AddHotkeyInput(std::ostringstream *html, const std::string &value)
{
    *html << "<label><span>全局热键</span><div class=\"input-action\"><input data-key=\"hotkey\" id=\"hotkeyInput\" "
          << "type=\"text\" readonly aria-readonly=\"true\" value=\"" << HtmlEscape(value)
          << "\"><button type=\"button\" id=\"recordHotkey\" class=\"secondary\">录制热键</button></div>"
          << "<small id=\"hotkeyHelp\">热键只能通过录制修改。点击录制后按组合键，例如 Ctrl+Shift+B；Esc 取消。</small></label>\n";
}

void AddSectionStart(std::ostringstream *html, const std::string &title, const std::string &desc)
{
    *html << "<section><h2>" << HtmlEscape(title) << "</h2><p>" << HtmlEscape(desc) << "</p><div class=\"grid\">\n";
}

void AddSectionEnd(std::ostringstream *html)
{
    *html << "</div></section>\n";
}

struct ObsidianFolderGroup
{
    std::string vault_key;
    std::string vault_path;
    std::string vault_label;
    std::vector<std::pair<std::string, std::string>> folders;
};

std::string ObsidianVaultLabel(const std::filesystem::path &path, const std::string &name)
{
    const std::string path_text = PathValue(path);
    return name.empty() ? path_text : (name + " - " + path_text);
}

std::string FindVaultNameForPath(const std::filesystem::path &path, const std::vector<ObsidianVault> &vaults)
{
    const std::string target_key = NormalizePathKey(path);
    if (target_key.empty())
    {
        return "";
    }
    for (const ObsidianVault &vault : vaults)
    {
        if (NormalizePathKey(vault.path) == target_key)
        {
            return vault.name;
        }
    }
    return "";
}

std::vector<std::pair<std::string, std::string>> BuildObsidianVaultOptions(
    const std::filesystem::path &current_vault_dir, const std::vector<ObsidianVault> &vaults)
{
    std::vector<std::pair<std::string, std::string>> options;
    std::set<std::string> seen;

    auto add_option = [&](const std::filesystem::path &path, const std::string &name)
    {
        const std::string path_text = PathValue(path);
        const std::string key = NormalizePathKey(path);
        if (path_text.empty() || key.empty() || !seen.insert(key).second)
        {
            return;
        }
        options.emplace_back(path_text, ObsidianVaultLabel(path, name));
    };

    add_option(current_vault_dir, FindVaultNameForPath(current_vault_dir, vaults));
    for (const ObsidianVault &vault : vaults)
    {
        add_option(vault.path, vault.name);
    }
    return options;
}

void AddVaultFolderOptions(const std::filesystem::path &vault_dir, std::set<std::string> *folders)
{
    constexpr std::size_t kMaxFolderOptions = 2000;
    constexpr std::size_t kMaxFolderScanEntries = 20000;
    if (vault_dir.empty())
    {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(vault_dir, ec) || !std::filesystem::is_directory(vault_dir, ec))
    {
        return;
    }

    auto add_folder = [&](const std::filesystem::path &path)
    {
        const std::filesystem::path relative = path.lexically_relative(vault_dir);
        if (relative.empty() || relative.is_absolute())
        {
            return;
        }
        bool safe = true;
        for (const std::filesystem::path &part : relative)
        {
            if (part == L"..")
            {
                safe = false;
                break;
            }
        }
        if (safe)
        {
            folders->insert(WideToUtf8(relative.generic_wstring()));
        }
    };

    std::size_t scanned_entries = 0;
    std::filesystem::directory_iterator top_level(
        vault_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::directory_iterator top_end;
    while (!ec && top_level != top_end && folders->size() < kMaxFolderOptions &&
           scanned_entries < kMaxFolderScanEntries)
    {
        ++scanned_entries;
        const std::filesystem::path path = top_level->path();
        const std::string name = ToLowerAscii(WideToUtf8(path.filename().wstring()));
        if (name == ".obsidian" || name == ".git" || name == "node_modules" || name == ".trash")
        {
            top_level.increment(ec);
            continue;
        }
        if (top_level->is_directory(ec))
        {
            add_folder(path);
        }
        ec.clear();
        top_level.increment(ec);
    }

    ec.clear();
    std::filesystem::recursive_directory_iterator it(
        vault_dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end && folders->size() < kMaxFolderOptions && scanned_entries < kMaxFolderScanEntries)
    {
        ++scanned_entries;
        const std::filesystem::path path = it->path();
        const std::string name = ToLowerAscii(WideToUtf8(path.filename().wstring()));
        if (name == ".obsidian" || name == ".git" || name == "node_modules" || name == ".trash")
        {
            it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        if (it->is_directory(ec))
        {
            add_folder(path);
        }
        ec.clear();
        it.increment(ec);
    }
}

std::vector<std::pair<std::string, std::string>> BuildObsidianFolderOptions(const std::filesystem::path &vault_dir)
{
    std::set<std::string> folders;
    AddVaultFolderOptions(vault_dir, &folders);

    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(folders.size());
    for (const std::string &folder : folders)
    {
        options.emplace_back(folder, folder);
    }
    return options;
}

std::vector<ObsidianFolderGroup> BuildObsidianFolderGroups(const std::filesystem::path &current_vault_dir,
                                                           const std::vector<ObsidianVault> &vaults)
{
    std::vector<ObsidianFolderGroup> groups;
    std::set<std::string> seen;

    auto add_group = [&](const std::filesystem::path &path, const std::string &name)
    {
        const std::string vault_key = NormalizePathKey(path);
        const std::string vault_path = PathValue(path);
        if (vault_key.empty() || vault_path.empty() || !seen.insert(vault_key).second)
        {
            return;
        }
        groups.push_back({vault_key, vault_path, ObsidianVaultLabel(path, name), BuildObsidianFolderOptions(path)});
    };

    add_group(current_vault_dir, FindVaultNameForPath(current_vault_dir, vaults));
    for (const ObsidianVault &vault : vaults)
    {
        add_group(vault.path, vault.name);
    }
    return groups;
}

std::vector<std::pair<std::string, std::string>> CurrentObsidianFolderOptions(
    const std::vector<ObsidianFolderGroup> &groups, const std::filesystem::path &current_vault_dir)
{
    const std::string current_key = NormalizePathKey(current_vault_dir);
    for (const ObsidianFolderGroup &group : groups)
    {
        if (group.vault_key == current_key)
        {
            return group.folders;
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> AddRootFolderOption(
    const std::vector<std::pair<std::string, std::string>> &options)
{
    std::vector<std::pair<std::string, std::string>> output;
    output.reserve(options.size() + 1);
    output.emplace_back("", "仓库根目录");
    output.insert(output.end(), options.begin(), options.end());
    return output;
}

std::string BuildObsidianFolderGroupsJson(const std::vector<ObsidianFolderGroup> &groups)
{
    std::ostringstream json;
    json << "[";
    bool first_group = true;
    for (const ObsidianFolderGroup &group : groups)
    {
        if (!first_group)
        {
            json << ",";
        }
        first_group = false;
        json << "{\"key\":\"" << EscapeJson(group.vault_key) << "\",\"path\":\"" << EscapeJson(group.vault_path)
             << "\",\"label\":\"" << EscapeJson(group.vault_label) << "\",\"folders\":[";
        bool first_folder = true;
        for (const auto &folder : group.folders)
        {
            if (!first_folder)
            {
                json << ",";
            }
            first_folder = false;
            json << "{\"value\":\"" << EscapeJson(folder.first) << "\",\"label\":\"" << EscapeJson(folder.second)
                 << "\"}";
        }
        json << "]}";
    }
    json << "]";
    return json.str();
}
}

std::filesystem::path WriteConfigPage(const AppConfig &config, const std::filesystem::path &config_path)
{
    const std::filesystem::path output_path = config.state_dir / L"notion-clipboard-config.html";
    std::filesystem::create_directories(output_path.parent_path());
    const std::vector<ObsidianVault> obsidian_vaults = DiscoverObsidianVaults();
    const std::vector<ObsidianFolderGroup> obsidian_folder_groups =
        BuildObsidianFolderGroups(config.obsidian_vault_dir, obsidian_vaults);
    const std::vector<std::pair<std::string, std::string>> current_obsidian_folder_options =
        CurrentObsidianFolderOptions(obsidian_folder_groups, config.obsidian_vault_dir);

    std::ostringstream html;
    html << R"(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Notion Clipboard Win 配置</title>
<style>
:root{color-scheme:light dark;--bg:#f6f7f9;--panel:#fff;--text:#172033;--muted:#667085;--line:#d9dee8;--accent:#1f6feb;--accent2:#0f766e;--danger:#b42318;--ok:#047857}
@media (prefers-color-scheme:dark){:root{--bg:#111827;--panel:#182233;--text:#edf2f7;--muted:#9aa8bd;--line:#324055;--accent:#5aa2ff;--accent2:#2dd4bf;--danger:#f87171;--ok:#34d399}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 "Segoe UI",system-ui,sans-serif}
header{position:sticky;top:0;z-index:2;background:color-mix(in srgb,var(--panel) 92%,transparent);border-bottom:1px solid var(--line);backdrop-filter:blur(8px)}
.bar{max-width:1180px;margin:auto;padding:16px 20px;display:flex;gap:12px;align-items:center;justify-content:space-between}
h1{font-size:20px;margin:0}.path{color:var(--muted);font-size:12px;word-break:break-all}.wrap{max-width:1180px;margin:0 auto;padding:20px;display:grid;grid-template-columns:minmax(0,1.1fr) minmax(360px,.9fr);gap:18px}
section,.output{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:16px;margin-bottom:16px}h2{font-size:15px;margin:0 0 4px}p{margin:0 0 12px;color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}label{display:grid;gap:5px}label span{font-weight:600}input,select,textarea{width:100%;border:1px solid var(--line);background:var(--bg);color:var(--text);border-radius:6px;padding:9px 10px;font:inherit}
small{color:var(--muted)}.check{grid-template-columns:auto 1fr;align-items:start}.check input{width:auto;margin-top:3px}.check small{grid-column:2}.wide{grid-column:1/-1}.location{border:1px solid var(--line);border-left:3px solid var(--accent2);border-radius:6px;padding:9px 10px;background:var(--bg);color:var(--muted);word-break:break-all}.location strong{color:var(--text)}
.input-action{display:grid;grid-template-columns:minmax(0,1fr) auto;gap:8px;align-items:center}.input-action button{white-space:nowrap}.input-action input[readonly]{cursor:pointer}
.actions{display:flex;gap:8px;flex-wrap:wrap}button,.download{border:0;border-radius:6px;background:var(--accent);color:white;padding:9px 12px;font-weight:600;cursor:pointer;text-decoration:none}button:disabled{opacity:.5;cursor:not-allowed}.secondary{background:var(--accent2)}
textarea{min-height:300px;resize:vertical;font-family:Consolas,monospace;font-size:12px}.target{display:flex;gap:10px;align-items:flex-start}.target-list{display:flex;gap:8px;flex-wrap:wrap;max-width:780px}.target-list label{display:flex;grid-template-columns:none;gap:5px;align-items:center;border:1px solid var(--line);border-radius:6px;padding:6px 8px;background:var(--bg);font-size:12px}.target-list input{width:auto}.hint{border-left:3px solid var(--accent);padding-left:10px;color:var(--muted)}.advanced{margin-top:14px;border-top:1px solid var(--line);padding-top:12px}.advanced summary{cursor:pointer;font-weight:700}.advanced p{margin:8px 0 10px}
.status{margin:10px 0 12px;border:1px solid var(--line);border-left-width:3px;border-radius:6px;padding:9px 10px;color:var(--muted);background:var(--bg)}.status.ok{border-left-color:var(--ok);color:var(--ok)}.status.error{border-left-color:var(--danger);color:var(--danger)}
@media (max-width:900px){.wrap{grid-template-columns:1fr}.grid{grid-template-columns:1fr}.bar{align-items:flex-start;flex-direction:column}}
</style>
</head>
<body>
<header><div class="bar"><div><h1>Notion Clipboard Win 配置</h1><div class="path">配置文件：)"
         << HtmlEscape(WideToUtf8(config_path.wstring())) << R"(</div></div><div class="target"><strong>保存到</strong><input data-key="upload_target" id="target" type="hidden"><div class="target-list" id="targetList">
<label><input type="checkbox" value="notion" data-target-option="notion">Notion</label><label><input type="checkbox" value="obsidian" data-target-option="obsidian">Obsidian</label>
</div></div></div></header>
<main class="wrap"><div>
)";

    AddSectionStart(&html, "Notion", "保存为 Notion 数据源里的页面。");
    AddInput(&html, "notion_token", "Notion Token", config.notion_token, "secret_xxx", "password");
    AddInput(&html, "data_source_id", "Notion 数据源 ID", config.data_source_id);
    html << "<details class=\"advanced\"><summary>高级：Notion 属性</summary><p>标题属性通常会自动识别；旧 Database ID 和内容预览属性仅在特殊数据库结构中需要。</p>\n";
    AddInput(&html, "database_id", "Database ID", config.database_id, "仅保留旧配置兼容。");
    AddInput(&html, "title_property_name", "标题属性", config.title_property_name);
    AddInput(&html, "content_property_name", "内容预览属性", config.content_property_name);
    AddInput(&html, "created_time_property_name", "创建时间属性", config.created_time_property_name);
    AddInput(&html, "content_property_max_chars", "内容预览最大字符", std::to_string(config.content_property_max_chars),
             "", "number");
    html << "</details>\n";
    AddSectionEnd(&html);

    AddSectionStart(&html, "Obsidian", "写入指定 Obsidian 仓库的 Markdown 笔记。");
    AddSelectWithCustomInput(&html, "obsidian_vault_dir", "Obsidian 仓库", PathValue(config.obsidian_vault_dir),
                             BuildObsidianVaultOptions(config.obsidian_vault_dir, obsidian_vaults),
                             "手动填写路径...", "E:\\obsidian\\第一个库",
                             "选择已注册仓库；没有列出时可手动填写路径。");
    AddSelectWithCustomInput(&html, "obsidian_folder", "Obsidian 子目录", config.obsidian_folder,
                             AddRootFolderOption(current_obsidian_folder_options),
                             "新建/手动输入...", "Inbox/Clipboard", "选择已有目录；手动输入的新目录会自动创建。");
    AddInput(&html, "obsidian_tags", "Obsidian 标签", config.obsidian_tags, "可选，逗号/空格分隔，例如 algorithm cpp");
    html << "<div class=\"wide location\" id=\"obsidianLocation\"></div>\n";
    AddSectionEnd(&html);

    html << "<section><h2>未来支持</h2><p>Webhook、语雀和飞书文档暂不作为当前稳定保存位置，后续会在 Notion 与 Obsidian 体验稳定后继续打磨。</p></section>\n";

    AddSectionStart(&html, "应用行为", "常用开关；高级运行参数保持默认即可。");
    AddHotkeyInput(&html, config.hotkey);
    AddCheckbox(&html, "enable_hotkey", "启用全局热键", config.enable_hotkey);
    AddCheckbox(&html, "tray_notifications", "托盘通知", config.tray_notifications);
    AddCheckbox(&html, "start_with_windows", "开机自动启动", config.start_with_windows);
    html << "<details class=\"advanced\"><summary>高级：性能和重试</summary><p>这些参数用于限流、队列和本地状态保存。一般不需要修改。</p>\n";
    AddInput(&html, "state_dir", "状态目录", PathValue(config.state_dir));
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
    html << "</details>\n";
    AddSectionEnd(&html);

    html << R"(
</div><aside class="output"><h2>保存配置</h2><p class="hint">完成后点击“应用并重启”，托盘进程会读取新设置。需要先试写一条内容时，点击“测试上传”。</p><div id="status" class="status" role="status"></div><div class="actions"><button id="apply">应用并重启</button><button id="testUpload" class="secondary">测试上传</button><button id="openUploadCenter" class="secondary">上传中心</button><button id="refreshObs">重新扫描 Obsidian</button><button id="reveal" class="secondary">显示/隐藏 token</button></div><details class="advanced"><summary>高级：查看或导出 ini</summary><p>这里包含 token，仅用于手动备份或调试。</p><div class="actions"><button id="copy">复制配置</button><a id="download" class="download secondary" download="notion_clipboard_win.ini">下载 ini</a></div><textarea id="ini" readonly spellcheck="false"></textarea></details></aside></main>
<script>
const order=["upload_target","notion_token","data_source_id","database_id","title_property_name","content_property_name","content_property_max_chars","created_time_property_name","obsidian_vault_dir","obsidian_folder","obsidian_tags","state_dir","hotkey","enable_hotkey","tray_notifications","start_with_windows","duplicate_suppression_ms","max_clipboard_bytes","min_request_interval_ms","append_batch_size","max_retry_attempts","http_retry_attempts"];
const configPath=)"
         << '"' << EscapeJson(WideToUtf8(config_path.wstring())) << '"' << R"(;
const obsidianFolderGroups=)"
         << BuildObsidianFolderGroupsJson(obsidian_folder_groups) << R"(;
function normalizePathKey(path){return (path||"").trim().replace(/\//g,"\\").replace(/[\\]+$/,"").toLowerCase();}
const obsidianFolderGroupMap=new Map();
obsidianFolderGroups.forEach(group=>{if(group.path)obsidianFolderGroupMap.set(group.path,group); if(group.key)obsidianFolderGroupMap.set(group.key,group); const normalized=normalizePathKey(group.path); if(normalized)obsidianFolderGroupMap.set(normalized,group);});
function findObsidianFolderGroup(vault){return obsidianFolderGroupMap.get(vault)||obsidianFolderGroupMap.get(normalizePathKey(vault))||null;}
const target=document.getElementById("target");
const initialTargets=new Set()"
         << '"' << HtmlEscape(config.upload_target) << '"' << R"(.split(/[\s,;|]+/).filter(Boolean));
const targetChecks=[...document.querySelectorAll("[data-target-option]")];
const statusBox=document.getElementById("status");
const applyButton=document.getElementById("apply");
const iniBox=document.getElementById("ini");
const downloadLink=document.getElementById("download");
let downloadUrl="";
function targetValue(el){return el.dataset.targetOption||el.value;}
targetChecks.forEach(el=>el.checked=initialTargets.has(targetValue(el)));
if(!targetChecks.some(el=>el.checked)&&targetChecks.length)targetChecks[0].checked=true;
function val(key){const el=document.querySelector(`[data-key="${key}"]`); if(!el)return ""; return el.type==="checkbox"?(el.checked?"true":"false"):el.value.trim();}
function selectedTargets(){return targetChecks.filter(el=>el.checked).map(targetValue);}
function isHotkeyTextValid(text){const tokens=(text||"").split("+").map(part=>part.trim()).filter(Boolean); if(tokens.length<2)return false; let modifiers=0; let keys=0; const keyNames=new Set(["backspace","delete","del","down","end","enter","esc","escape","home","insert","ins","left","pagedown","pageup","pgdn","pgup","pause","printscreen","prtsc","right","space","tab","up"]); for(const raw of tokens){const token=raw.toLowerCase(); if(token==="ctrl"||token==="control"||token==="alt"||token==="shift"||token==="win"||token==="windows"||token==="super"||token==="meta"){modifiers++; continue;} if(/^[a-z0-9]$/.test(token)||/^f([1-9]|1[0-9]|2[0-4])$/.test(token)||keyNames.has(token)){keys++; continue;} return false;} return modifiers>0&&keys===1;}
function validateConfig(){const selected=selectedTargets(); const problems=[]; if(!selected.length)problems.push("至少选择一个保存位置"); if(!isHotkeyTextValid(val("hotkey")))problems.push("全局热键格式无效，请重新录制类似 Ctrl+Shift+B 的组合键"); if(selected.includes("notion")){if(!val("notion_token"))problems.push("Notion Token 不能为空"); if(!val("data_source_id")&&!val("database_id"))problems.push("Notion 需要数据源 ID 或 Database ID");} if(selected.includes("obsidian")&&!val("obsidian_vault_dir"))problems.push("Obsidian 仓库不能为空"); return problems;}
function updateStatus(){const selected=selectedTargets(); const problems=validateConfig(); statusBox.className="status "+(problems.length?"error":"ok"); statusBox.textContent=problems.length?("需要处理："+problems.join("；")):("配置完整。保存后会写入："+selected.join("、")); applyButton.disabled=problems.length>0;}
function build(){const text=order.map(k=>`${k}=${val(k)}`).join("\n")+"\n"; iniBox.value=text; if(downloadUrl)URL.revokeObjectURL(downloadUrl); downloadUrl=URL.createObjectURL(new Blob([text],{type:"text/plain;charset=utf-8"})); downloadLink.href=downloadUrl; updateStatus(); updateObsidianLocation();}
function protocolUrl(action){return "notion-clipboard-win:/"+action+"/?path="+encodeURIComponent(configPath);}
function protocolUrlWithOutput(action){return protocolUrl(action)+"&content="+encodeURIComponent(iniBox.value);}
function syncTargets(){const selected=selectedTargets(); target.value=selected.join(","); build();}
const CUSTOM_PICKER_VALUE="__custom__";
const obsidianVaultInput=document.querySelector('[data-key="obsidian_vault_dir"]');
const obsidianFolderInput=document.querySelector('[data-key="obsidian_folder"]');
const obsidianFolderSelect=document.querySelector('[data-choice-target="obsidian_folder"]');
const obsidianFolderCustom=document.querySelector('[data-custom-key="obsidian_folder"]');
const obsidianLocation=document.getElementById("obsidianLocation");
function appendSelectOption(parent,value,text){const option=document.createElement("option"); option.value=value; option.textContent=text; parent.appendChild(option); return option;}
function selectHasValue(select,value){return [...select.options].some(option=>option.value===value);}
function joinObsidianPath(vault,folder){vault=(vault||"").trim(); folder=(folder||"").trim().replace(/^[\\/]+/,"").replace(/[\\/]+$/,""); if(!vault)return ""; if(!folder)return vault; return vault.replace(/[\\/]+$/,"")+"\\"+folder.replace(/[\\/]+/g,"\\");}
function updateObsidianLocation(){if(!obsidianLocation)return; const vault=(obsidianVaultInput?.value||"").trim(); const folder=(obsidianFolderInput?.value||"").trim(); const location=joinObsidianPath(vault,folder); const group=findObsidianFolderGroup(vault); const count=group?(group.folders||[]).length:0; const scanText=group?`已扫描 ${count} 个子目录`:"未匹配到已注册仓库，可继续手动填写路径"; obsidianLocation.textContent=location?`Obsidian 写入位置：${location}（${scanText}）`:"Obsidian 写入位置：尚未选择仓库";}
function syncChoice(select){const key=select.dataset.choiceTarget; const hidden=document.querySelector(`[data-key="${key}"]`); const custom=document.querySelector(`[data-custom-key="${key}"]`); if(!hidden)return; if(select.value===CUSTOM_PICKER_VALUE){if(custom){custom.hidden=false; hidden.value=custom.value.trim();}}else{hidden.value=select.value; if(custom)custom.hidden=true;} if(key==="obsidian_vault_dir")refreshObsidianFolders(); build();}
function refreshObsidianFolders(){if(!obsidianFolderSelect||!obsidianFolderInput)return; const vault=(obsidianVaultInput?.value||"").trim(); const group=findObsidianFolderGroup(vault); const folders=group?(group.folders||[]):[]; const current=obsidianFolderInput.value.trim(); obsidianFolderSelect.innerHTML=""; appendSelectOption(obsidianFolderSelect,"","仓库根目录"); folders.forEach(folder=>appendSelectOption(obsidianFolderSelect,folder.value,folder.label)); appendSelectOption(obsidianFolderSelect,CUSTOM_PICKER_VALUE,folders.length?"新建/手动输入...":"手动输入/新建子目录..."); if(selectHasValue(obsidianFolderSelect,current)){obsidianFolderSelect.value=current; if(obsidianFolderCustom)obsidianFolderCustom.value="";}else{obsidianFolderSelect.value=CUSTOM_PICKER_VALUE; if(obsidianFolderCustom)obsidianFolderCustom.value=current;} syncChoice(obsidianFolderSelect);}
targetChecks.forEach(el=>el.addEventListener("change",syncTargets));
document.querySelectorAll("[data-key]").forEach(el=>{el.addEventListener("input",build);el.addEventListener("change",build);});
document.querySelectorAll("[data-choice-target]").forEach(select=>select.addEventListener("change",()=>syncChoice(select)));
document.querySelectorAll("[data-custom-key]").forEach(custom=>custom.addEventListener("input",()=>{const key=custom.dataset.customKey; const select=document.querySelector(`[data-choice-target="${key}"]`); const hidden=document.querySelector(`[data-key="${key}"]`); if(select&&select.value===CUSTOM_PICKER_VALUE&&hidden){hidden.value=custom.value.trim(); if(key==="obsidian_vault_dir")refreshObsidianFolders(); build();}}));
applyButton.addEventListener("click",()=>{build(); const problems=validateConfig(); if(problems.length){updateStatus(); return;} statusBox.className="status ok"; statusBox.textContent="已发送配置给托盘应用，应用会校验、写入并重启。"; location.href="notion-clipboard-win:/apply-config/?path="+encodeURIComponent(configPath)+"&content="+encodeURIComponent(iniBox.value);});
document.getElementById("testUpload").addEventListener("click",()=>{build(); const problems=validateConfig(); if(problems.length){updateStatus(); return;} statusBox.className="status ok"; statusBox.textContent="已发送测试上传请求，结果会写入并打开上传中心。"; location.href=protocolUrlWithOutput("test-upload");});
document.getElementById("openUploadCenter").addEventListener("click",()=>{location.href=protocolUrl("open-upload-center");});
document.getElementById("refreshObs").addEventListener("click",()=>{location.href=protocolUrl("open-config-page"); setTimeout(()=>location.reload(),1200);});
document.getElementById("copy").addEventListener("click",async()=>{build(); await navigator.clipboard.writeText(iniBox.value);});
const hotkeyInput=document.getElementById("hotkeyInput");
const hotkeyHelp=document.getElementById("hotkeyHelp");
const recordHotkeyButton=document.getElementById("recordHotkey");
let recordingHotkey=false;
function recordedKeyLabel(event){const modifierKeys=new Set(["Control","Shift","Alt","Meta","OS"]); if(modifierKeys.has(event.key))return ""; if(event.code&&event.code.startsWith("Key")&&event.code.length===4)return event.code.slice(3).toUpperCase(); if(event.code&&event.code.startsWith("Digit")&&event.code.length===6)return event.code.slice(5); if(event.code&&event.code.startsWith("Numpad")&&event.code.length===7&&/^[0-9]$/.test(event.code.slice(6)))return event.code.slice(6); if(/^F([1-9]|1[0-9]|2[0-4])$/.test(event.key))return event.key.toUpperCase(); const names={ArrowLeft:"Left",ArrowRight:"Right",ArrowUp:"Up",ArrowDown:"Down",PageUp:"PageUp",PageDown:"PageDown",Escape:"Esc"," ":"Space",Spacebar:"Space",Enter:"Enter",Return:"Enter",Tab:"Tab",Backspace:"Backspace",Delete:"Delete",Insert:"Insert",Home:"Home",End:"End",Pause:"Pause",PrintScreen:"PrintScreen"}; if(names[event.key])return names[event.key]; if(event.key&&/^[a-zA-Z0-9]$/.test(event.key))return event.key.toUpperCase(); return "";}
function formatRecordedHotkey(event){const parts=[]; if(event.ctrlKey)parts.push("Ctrl"); if(event.altKey)parts.push("Alt"); if(event.shiftKey)parts.push("Shift"); if(event.metaKey)parts.push("Win"); const key=recordedKeyLabel(event); if(!key)return ""; if(parts.length===0)return ""; parts.push(key); return parts.join("+");}
function stopHotkeyRecording(message){recordingHotkey=false; document.removeEventListener("keydown",handleRecordedHotkey,true); recordHotkeyButton.textContent="录制热键"; if(message)hotkeyHelp.textContent=message;}
function handleRecordedHotkey(event){if(!recordingHotkey)return; event.preventDefault(); event.stopPropagation(); if(event.key==="Escape"){stopHotkeyRecording("已取消录制。点击录制后按组合键，例如 Ctrl+Shift+B。"); return;} const display=formatRecordedHotkey(event); if(!display){hotkeyHelp.textContent="请按 Ctrl、Alt、Shift 或 Win 加一个主按键。Esc 取消。"; return;} hotkeyInput.value=display; const enable=document.querySelector('[data-key="enable_hotkey"]'); if(enable)enable.checked=true; stopHotkeyRecording(`已录制：${display}`); build();}
function startHotkeyRecording(){if(recordingHotkey)return; recordingHotkey=true; recordHotkeyButton.textContent="录制中..."; hotkeyHelp.textContent="请按新的组合键，例如 Ctrl+Alt+N；Esc 取消。"; hotkeyInput.focus(); hotkeyInput.select(); document.addEventListener("keydown",handleRecordedHotkey,true);}
recordHotkeyButton.addEventListener("click",startHotkeyRecording);
hotkeyInput.addEventListener("click",startHotkeyRecording);
document.getElementById("reveal").addEventListener("click",()=>document.querySelectorAll('input[type="password"],input[data-was-password]').forEach(el=>{if(el.type==="password"){el.dataset.wasPassword="1";el.type="text"}else{el.type="password"}}));
document.querySelectorAll("[data-choice-target]").forEach(syncChoice);
syncTargets();
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
        config.upload_target = "notion,obsidian";
        config.notion_token = "notion_secret";
        config.obsidian_vault_dir = root / L"vault";
        config.obsidian_tags = "algorithm cpp";
        std::filesystem::create_directories(config.obsidian_vault_dir / L"aaa");
        std::filesystem::create_directories(config.obsidian_vault_dir / L"Clipboard");
        const std::filesystem::path second_vault_dir = root / L"vault2";
        std::filesystem::create_directories(second_vault_dir / L"secondOnly");

        ObsidianVault second_vault;
        second_vault.id = "second";
        second_vault.name = "Second Vault";
        second_vault.path = second_vault_dir;
        const std::vector<ObsidianVault> fake_vaults{second_vault};
        const std::vector<ObsidianFolderGroup> folder_groups =
            BuildObsidianFolderGroups(config.obsidian_vault_dir, fake_vaults);
        auto has_folder = [](const ObsidianFolderGroup &group, const std::string &folder)
        {
            return std::any_of(group.folders.begin(), group.folders.end(),
                               [&](const auto &entry)
                               {
                                   return entry.first == folder;
                               });
        };
        if (folder_groups.size() != 2 || !has_folder(folder_groups[0], "aaa") ||
            has_folder(folder_groups[0], "secondOnly") || !has_folder(folder_groups[1], "secondOnly") ||
            has_folder(folder_groups[1], "aaa"))
        {
            fail("obsidian folder options were not scoped by vault");
        }

        const std::filesystem::path page = WriteConfigPage(config, root / L"notion_clipboard_win.ini");
        if (!std::filesystem::exists(page))
        {
            fail("page file was not created");
        }
        else
        {
            const std::string html = ReadWholeFile(page);
            for (const char *needle : {"notion_secret", "value=\"obsidian\" data-target-option=\"obsidian\"",
                                       "value=\"notion\" data-target-option=\"notion\"", "未来支持", "Webhook、语雀和飞书文档",
                                       "notion,obsidian", "<strong>保存到</strong>", "至少选择一个保存位置",
                                       "Notion 数据源 ID", "Notion 需要数据源 ID 或 Database ID",
                                       "高级：Notion 属性", "标题属性通常会自动识别",
                                       "data-key=\"database_id\"", "data-key=\"title_property_name\"",
                                       "data-key=\"content_property_name\"", "data-key=\"created_time_property_name\"",
                                       "data-key=\"content_property_max_chars\"",
                                       "data-choice-target=\"obsidian_vault_dir\"", "Obsidian 仓库",
                                       "data-choice-target=\"obsidian_folder\"", "data-custom-key=\"obsidian_vault_dir\"",
                                       "data-custom-key=\"obsidian_folder\"", "新建/手动输入...", "value=\"aaa\"",
                                       "仓库根目录", "未匹配到已注册仓库", "尚未选择仓库",
                                       "data-key=\"obsidian_tags\"", "Obsidian 标签", "algorithm cpp",
                                       "const obsidianFolderGroups=", "\"key\":", "obsidianFolderGroupMap",
                                       "normalizePathKey(path)", "findObsidianFolderGroup(vault)",
                                        "CUSTOM_PICKER_VALUE", "id=\"obsidianLocation\"", "Obsidian 写入位置：",
                                        "joinObsidianPath(vault,folder)", "updateObsidianLocation()",
                                        "refreshObsidianFolders()", "重新扫描 Obsidian",
                                        "protocolUrl(\"open-config-page\")", "targetValue(el)", "syncTargets()",
                                         "保存配置", "高级：查看或导出 ini", "这里包含 token，仅用于手动备份或调试。",
                                         "id=\"ini\" readonly spellcheck=\"false\"", "URL.revokeObjectURL(downloadUrl)",
                                         "id=\"copy\"", "复制配置",
                                         "id=\"download\"", "下载 ini", "id=\"testUpload\"", "测试上传",
                                         "protocolUrlWithOutput(\"test-upload\")",
                                         "已发送测试上传请求，结果会写入并打开上传中心。",
                                         "id=\"openUploadCenter\"", "上传中心",
                                         "protocolUrl(\"open-upload-center\")",
                                       "id=\"status\"", "validateConfig()", "applyButton.disabled",
                                       "配置完整。保存后会写入：", "需要处理：",
                                       "id=\"hotkeyInput\"", "readonly aria-readonly=\"true\"", "id=\"recordHotkey\"",
                                       "录制热键", "热键只能通过录制修改",
                                       "高级：性能和重试", "这些参数用于限流、队列和本地状态保存。",
                                       "data-key=\"state_dir\"", "data-key=\"duplicate_suppression_ms\"",
                                       "data-key=\"max_clipboard_bytes\"", "data-key=\"min_request_interval_ms\"",
                                       "data-key=\"append_batch_size\"", "data-key=\"max_retry_attempts\"",
                                       "data-key=\"http_retry_attempts\"",
                                       "recordedKeyLabel(event)", "formatRecordedHotkey(event)",
                                       "handleRecordedHotkey(event)", "startHotkeyRecording()",
                                       "hotkeyInput.addEventListener(\"click\",startHotkeyRecording)",
                                       "请按新的组合键，例如 Ctrl+Alt+N；Esc 取消。",
                                       "isHotkeyTextValid(text)", "全局热键格式无效，请重新录制",
                                       "notion-clipboard-win:/apply-config/", "应用并重启"})
            {
                if (html.find(needle) == std::string::npos)
                {
                    fail(std::string("missing expected config page content: ") + needle);
                }
            }
            for (const char *needle : {"data-target-option=\"yuque\"", "data-target-option=\"feishu_doc\"",
                                        "data-target-option=\"webhook\"", "yuque_namespace", "feishu_app_secret",
                                        "webhook_url", "markdown_output_dir", "enable_clipboard_listener",
                                        "自动监听剪贴板", "debounce_ms", "id=\"previewObsidian\"", "id=\"obsidianPreview\"",
                                        "预览 Obsidian Markdown", "preview-obsidian-clipboard",
                                        "upload_initial_clipboard", "启动后上传当前剪贴板",
                                        "id=\"validateOutputConfig\"", "查看配置诊断", "id=\"openConfigDiagnostics\"",
                                        "输出 ini", "页面会输出包含 token 的完整配置", "上传后端",
                                        "配置完整。保存后将上传到：",
                                        "Obsidian Vault", "Vault 根目录", "未匹配到已注册 vault",
                                        "尚未选择 vault"})
            {
                if (html.find(needle) != std::string::npos)
                {
                    fail(std::string("found future-only config page content: ") + needle);
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
