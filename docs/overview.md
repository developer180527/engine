---
status: unreviewed
---
Engine Architecture

Engine is a real-time application platform built around:

- Flecs ECS
- bgfx renderer
- Plugin-based services
- Reflection-driven tooling
- Asset cooking pipeline

High-level flow:

Editor
    ↓
Scene Data
    ↓
Runtime World
    ↓
Render Extraction
    ↓
Render View
    ↓
Render Pipeline