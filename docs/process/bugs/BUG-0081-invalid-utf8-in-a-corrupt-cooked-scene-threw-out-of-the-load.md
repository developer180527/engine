## BUG-0081 — Invalid UTF-8 in a corrupt cooked scene threw out of the load
- found:     2026-11-27
- status:    fixed
- class:     parse
- where:     src/scene/entity_serializer.h, src/scene/reflected_serde.h, src/core/json_read.h
- symptom:   a cooked scene whose payload was damaged so that a text string held invalid UTF-8 made SceneService::loadScene throw `[json.exception.type_error.316] invalid UTF-8 byte`, out of the player's scene load. Reachable since WO-091 made the cooked scene CBOR. Found by fuzz_scene_service_test once its generator corrupted the payload with the header kept consistent; earlier generators only truncated the file, which the container's size check refuses before any payload is read.
- cause:     CBOR text strings are taken as bytes when read, by nlohmann and by the in-place reader alike, with no UTF-8 check. The load then turns some loaded values back into JSON text (an unknown component kept as text, an unresolved mesh's authored reference, a kit's pending block), and nlohmann's dump() throws on invalid UTF-8. The editor never met it: its .scene is JSON text, which the parser checks.
- pinned-by: tests/fuzz_scene_service_test.cpp
- lane:      fuzz-regress
- proof:     the four seeds in tests/fuzz/corpus/scene_service/regressions.seeds threw before and pass after. Every load-side dump goes through jsonread::dumpLoaded, which replaces invalid bytes with U+FFFD; the file is already corrupt, and the rest of it still loads.
