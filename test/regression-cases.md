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

## Markdown and LaTeX

Common AI answer content may include inline `$...$`, `\(...\)`, display `$$...$$`, headings, and code.

Expected:

- Inline math becomes equation rich text.
- Display math becomes equation block.
- C++ code uses Notion `c++` language.

## Tasks, Quotes, Tables

Markdown tasks, quote blocks, and simple tables should not crash upload.

Expected:

- Tasks become Notion to-do blocks.
- Quotes become Notion quote blocks.
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
- Dollar signs inside URL/path segments are not treated as equations.
- Multi-backtick code spans preserve literal backticks inside the code.

## HTML Math Sources

Web pages may copy KaTeX or MathJax as HTML with visual spans plus hidden TeX annotations.

Expected:

- KaTeX `application/x-tex` annotations become Notion equations.
- MathJax `math/tex` script content becomes a Notion equation.
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
- HTML inline code inside list items stays inline and does not split the list item.
