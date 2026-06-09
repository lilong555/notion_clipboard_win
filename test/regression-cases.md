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
