---
status: unreviewed
---

# Standardized sections for info.md

## What Each info.md Should Contain:

1. Purpose — One paragraph. What problem does this system solve? Why does it exist?

2. Architecture — How the system is structured internally. Key classes and their responsibilities. Data flow diagram if applicable.

3. Public Interface — What other systems should use. Entry points, key types, key functions.

4. Internal Data Flow — The path data takes through the system (e.g., for animation: FBX → Assimp → Skeleton + Clips → Pose evaluation → Skin matrices → GPU uniform upload → Vertex shader).

5. Dependencies — What this system depends on (both internal and third-party).

6. Invariants / Constraints — Hard limits (128 bones, 4 influences per vertex, row-major matrices), threading rules, ordering requirements.

7. Known Limitations / Future Work — Honest list of what's not done.

## What info.md Should NOT Be:

1. Not API docs (Doxygen does that). 

2. Not a tutorial. Not a changelog. 

3. It's the document you hand to a new developer so they can modify the system without reading every file first.