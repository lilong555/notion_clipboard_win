# Conversion Regression Cases

This file documents clipboard text patterns covered by `--self-test`.

## Plain Algorithm Explanation

Plain Chinese algorithm notes with `dp[u]`, `u -> v`, `O(n + m)`, `1e5 * 1e9 = 1e14`.

Expected:

- No equation blocks.
- No code blocks.
- No empty fenced code blocks.

## HTML Empty Code Artifacts

HTML fragments may contain empty `pre` elements from rich text copy sources.

Expected:

- Empty fenced code blocks are removed.
- Plain text fallback remains available when HTML conversion is polluted.

## Code Language Normalization

Fenced code blocks from copy sources may use `text`, `txt`, `plain`, `cpp`, `py`, `sh`, or unknown languages.

Expected:

- `text` and unknown languages become Notion `plain text`.
- `cpp` becomes `c++`.
- `py` becomes `python`.
- SQL fences are normalized to Notion `sql`, including common dialect tags such as `postgresql`.
- Code block rich text is emitted as clean text objects without explicit annotations so Notion can apply syntax highlighting.

## Markdown and LaTeX

Common AI answer content may include inline `$...$`, `\(...\)`, display `$$...$$`, headings, code,
emphasis such as `*italic*`, GFM-style `~~strikethrough~~`, inline links such as
`[docs](https://example.com)`, and autolinks such as `<https://example.com>`.

Expected:

- Inline math becomes equation rich text.
- Multi-line and single-line display math becomes equation blocks.
- Standalone LaTeX environments such as `cases`, `pmatrix`, and `align` become equation blocks.
- ATX headings and conservative Setext headings become Notion heading blocks.
- Pandoc/MkDocs heading attribute lists such as `{#install .tabset}` are stripped from heading text.
- HTML `<h4>` through `<h6>` headings degrade to Notion `heading_3`.
- LaTeX-looking lines followed by `===` are not treated as Setext headings.
- Loose bracket math accepts simple ASCII formulas such as `r-l+1`, complexity formulas such as `O(n)`, and strips copied `# ` prefixes inside equation blocks.
- Loose bracket math also accepts heading-prefixed starts such as `## [` when the line contains only the bracket marker.
- Parenthesized inline text containing LaTeX commands, such as `(sum(t)\neq 0)` or `(sum \bmod 3)`, becomes equation rich text without converting ordinary `(t)` notes.
- Italic spans become Notion italic annotations without touching snake_case or code spans.
- Strikethrough spans become Notion strikethrough annotations without breaking inline math or code.
- HTML `<u>` and `++underline++` spans become Notion underline annotations without touching `C++` text.
- HTML `<mark>` spans become Notion `yellow_background` text annotations, while plain `a==b` text stays literal.
- Markdown `==highlight==` spans become Notion `yellow_background` text annotations without touching `a==b` or code spans.
- Common HTML foreground/background colors from `<span style="color:...">`, `<span style="background-color:...">`, and `<font color=...>` map to Notion text colors when supported.
- Common HTML CSS inline styles such as `font-weight`, `font-style`, and `text-decoration` become Notion bold, italic, underline, or strikethrough annotations when supported.
- HTML `<sup>` and `<sub>` use readable Unicode superscript/subscript when possible, otherwise fall back to `^(...)` and `_(...)`.
- Markdown inline links become Notion rich text links, while code spans containing link-like text stay literal.
- Markdown reference links such as `[docs][id]`, `[id][]`, and `[id]` resolve against link definitions and the definition lines are skipped.
- Markdown footnote references such as `[^1]` become readable superscript markers, and footnote definitions are preserved as list items.
- Markdown autolinks become Notion rich text links for supported URL schemes and email addresses.
- Standalone Markdown images and HTML `<img>` tags with `http/https` sources become Notion external image blocks; inline images fall back to linked alt text.
- HTML `<figure>` images use `<figcaption>` as the Notion image caption when available.
- Backslash escapes for ASCII punctuation render as literal punctuation in normal text, without changing code spans, formulas, or Windows paths.
- HTML anchors copied from rich clipboard content become the same Notion links when their URL scheme is supported.
- HTML `<strong>/<b>`, `<em>/<i>`, and `<del>/<s>` inline formatting is preserved through Markdown markers.
- HTML `<pre><code class="language-*">` blocks become Notion code blocks with the detected language.
- HTML `<hr>` becomes a Notion divider block.
- C++ code uses Notion `c++` language.

## Tasks, Quotes, Tables

Markdown tasks, quote blocks, and simple tables should not crash upload.

Expected:

- Tasks become Notion to-do blocks.
- HTML checkbox inputs at the start of list items become Notion to-do blocks.
- Quotes become Notion quote blocks.
- GitHub-style alert blockquotes such as `> [!NOTE]` become Notion callout blocks.
- Colon-fenced admonitions such as `:::warning ... :::` become Notion callout blocks.
- MkDocs/Python-Markdown admonitions such as `!!! note` and `??? tip` become Notion callout blocks.
- HTML `<blockquote>` content becomes Notion quote blocks while preserving inline formatting and math.
- HTML `<details>/<summary>` content becomes a Notion callout while preserving summary text, links, formatting, and math.
- HTML `<dl>/<dt>/<dd>` definition lists become scannable bulleted list items.
- Markdown definition lists using `term` followed by `: definition` become scannable bulleted list items.
- Markdown ordered lists with either `1.` or `1)` markers become Notion numbered list items.
- Simple HTML `<table>` structures become Notion native table blocks while preserving supported inline cell formatting.
- Escaped pipes such as `\|` and pipes inside code spans do not split Markdown table cells.
- Tables are preserved as `plain text` code blocks.

## Notion API Rich Text Limits

Notion API rejects oversized rich text payloads inside a single block.

Expected:

- A block contains at most 90 rich-text-like objects in local tests.
- A single block JSON stays under the local 400KB safety limit.
- Long paragraphs, long code blocks, and many inline equations are split across multiple Notion blocks.
- Append batches stay under the local 400KB request-body safety limit.

## Long Formula Fallback

Notion equation expressions have a small API limit compared with arbitrary copied math text.

Expected:

- Short formulas become equation rich text or equation blocks.
- Overlong display formulas are preserved as `latex` code blocks instead of causing HTTP 400.

## Literal Dollar Signs and HTML Entities

Algorithm notes and copied HTML may contain currency-like `$100` text or numeric entities.

Expected:

- Currency-style `$` text is not treated as an equation.
- Plain multi-letter uppercase tokens such as `$AAA$`, `$AA$`, and `$BB$` keep their text but drop the dollar markers, even when followed immediately by normal text.
- HTML decimal and hexadecimal numeric entities are decoded before conversion.
- Inline code containing `$...$` stays code text rather than becoming an equation.
- Windows paths stay plain text.

## HTML Non-Content Tags

Copied HTML fragments may include style, script, head, or svg content.

Expected:

- Non-content tag bodies are skipped.
- Script/style text is not uploaded and does not become the page title.

## Inline Code and URL Dollar Signs

HTML inline code and Markdown code spans may contain dollar signs that look like LaTeX.
URLs may also contain path segments such as `$metadata/$value`.

Expected:

- Inline code stays literal and keeps code annotation.
- HTML `<kbd>`, `<samp>`, and `<tt>` inline fragments are treated like inline code.
- Dollar signs inside URL/path segments are not treated as equations.
- Multi-backtick code spans preserve literal backticks inside the code.

## HTML Math Sources

Web pages may copy KaTeX or MathJax as HTML with visual spans plus hidden TeX annotations.

Expected:

- KaTeX `application/x-tex` annotations become Notion equations.
- MathJax `math/tex` script content becomes a Notion equation.
- MathML/HTML math attributes such as `alttext`, `data-tex`, and `data-latex` become Notion equations.
- Visual-only KaTeX/MathJax helper spans are not uploaded as duplicate text.

## Fence Edge Cases

Copied Markdown may include empty fences or code lines that start with fence characters.

Expected:

- Empty fenced code blocks do not produce empty Notion code blocks.
- A code line like ```` ``` not a close ```` does not close the current fenced block.

## HTML List Paragraphs

Rich clipboard HTML commonly wraps list item content as `<li><p>...</p></li>`.

Expected:

- The list marker and first paragraph text stay on the same Markdown line.
- HTML `<ol>` items become numbered list blocks; HTML `<ul>` items remain bulleted list blocks.
- HTML inline code inside list items stays inline and does not split the list item.
