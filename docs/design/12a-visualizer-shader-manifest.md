# 12a — Visualizer: shader manifest parser (folded into 12 and 11)

> **Status: folded (task 11.23, 2026-11-07).** The parser exists, with manifest format 2 (task 11.1), and the Visualizer loads its shaders through it at startup (task 11.22). Its design now lives in [12 §10](12-visualizer.md#10-shader-loading) and [11 §13](11-renderer.md#13-shaders-location-and-build). This file stays so that older links resolve; the draft is in the git history.

| Was | Now |
|---|---|
| §0 Summary, §1 Code placement | [12 §10](12-visualizer.md#10-shader-loading) |
| §2 The file format | [12 §10.1](12-visualizer.md#101-the-manifest-format) (formats 1 and 2) |
| §3 Interface, §4 Implementation notes | [12 §10.2](12-visualizer.md#102-the-parser) |
| §5 Shader loading at Visualizer startup | [12 §10.3](12-visualizer.md#103-startup), [11 §13](11-renderer.md#13-shaders-location-and-build) |
| §6 Testing | [12 §10.4](12-visualizer.md#104-testing) |
| §7 Q1 Unknown keys | Decided B: errors (12 §10.1) |
| §7 Q2 Is `xxh3` required? | Decided A: required, and format 2 entries require `bindings` (12 §10.1) |
| §7 Q3 Where the parser lives | Decided A for now: in the Visualizer, in `AVIS_VISUALIZER_RENDERING` (12 §10) |
