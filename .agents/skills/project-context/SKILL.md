---
name: project-context
description: Always activated in the irongingot project. Loads and maintains LLM_REPO_DOCS.md — every task starts by reading it, and updates it on any significant code change, keeping additions as compressed as possible.
---

# Project Context — irongingot

This skill is always active in this project. Follow these rules for every task:

## 1. Always read the context file first

At the start of every task, read `/mnt/trashpile/GitHub/irongingot/LLM_REPO_DOCS.md` before making any changes. It contains the canonical overview of the codebase — file roles, architecture, key datatypes, protocol flow, build targets, etc. Do not skip this step.

## 2. Update the context file on major changes

If your task results in any of the following, update `LLM_REPO_DOCS.md` to reflect the change:

- Adding or removing a source file, header, or build script
- Changing the thread model, packet flow, or chunk pipeline
- Adding/removing key datatypes, constants, or data structures
- Changing the protocol state machine or compression behavior
- Adding/removing config options
- Changing the build system or target compilers
- Adding/removing dependencies or third-party code
- Changing world.json format or save/load behavior

## 3. Compress additions mercilessly

When you add new entries to `LLM_REPO_DOCS.md`, keep them as small as possible:

- Use single-line entries or short bullet points
- Omit obvious details — the reader is an LLM that can infer
- Use the existing file's tone and level of brevity as your guide
- If a detail is already covered by an existing entry, don't duplicate it
- Prefer grouping related entries under existing sections over adding new sections
- If something is truly minor, skip it entirely — the file should only cover what's needed to orient a new LLM contributor

## 4. This skill is mandatory

Do not skip these steps even if the task seems trivial. LLM_REPO_DOCS.md is the single source of truth for LLM context in this project.
