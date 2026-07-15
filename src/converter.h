#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ncw
{
std::string NormalizeLineEndings(std::string text);
std::string TruncateUtf8(const std::string &text, std::size_t max_chars);
std::string CollapseWhitespace(const std::string &text);
std::string StripNonMathDollarMarkersForPlainText(const std::string &text);
std::string NormalizeMarkdownForObsidian(const std::string &text);
std::string BuildTitleFromContent(const std::string &content);
std::string BuildTextRichText(const std::string &text, bool bold = false, bool code = false,
                              bool strikethrough = false, const std::string &link_url = "", bool italic = false,
                              bool underline = false, const std::string &color = "default");

std::vector<std::string> BuildTextBlocks(const std::string &content);
std::string BuildNotionBlocksDebugJson(const std::string &content);
std::size_t SelectAppendBatchEnd(const std::vector<std::string> &blocks, std::size_t begin, std::size_t max_blocks,
                                 std::size_t max_request_bytes);

bool HasEmptyMarkdownCodeFenceArtifact(const std::string &text);
std::string HtmlFragmentToMarkdown(std::string html);
std::optional<std::string> ExtractCfHtmlFragment(const std::string &html);

int RunSelfTest();
int RunDryRunText(const std::string &text);
}
