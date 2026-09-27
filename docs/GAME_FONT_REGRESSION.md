# Paired game-text font regression

The user reported missing Chinese characters in both delivered test applications and confirmed that the problem was inside the game, not the Esc interface. This is a shared delivery regression relative to the earlier complete-text setup. Seeing the same warnings in both roles did not make it harmless.

## Cause

The earlier working experiment settings explicitly selected `text.font=/System/Library/Fonts/STHeiti Light.ttc`. Their logs identify Heiti TC Light with Hiragino Sans W4 as fallback. The paired assembler seeded only `ui.language=zh-CN`, losing the game-font choice when creating isolated settings. Interface language and the font used by the game's sceLibFont path are separate settings.

Without an explicit choice, the unchanged production font loader uses the first available Japanese system face. On this Mac that is Hiragino Sans W4. Its fallback search stops once that face loads, so the bundled Noto font is not an additional per-glyph fallback. The two user logs explicitly list 23 missing code points left blank, including the characters in the game's save/loading and village text.

The game-font implementation has no changes from the delivered `75ca5ff` source. The defect is the missing paired configuration, not a newly changed native helper.

## Fix and evidence

The Mac pair assembler now seeds both preflight and final launch configurations with the same explicit game font. Its default is the previously verified STHeiti Light face; `--game-font` permits an intentional alternative. The pair manifest binds the font path and SHA-256, and readiness rejects a missing, changed or role-inconsistent font. This uses the installed system font in place; it is not redistributed.

`mhp3rd_game_font_coverage` uses the production metrics and rasterizer without SDL, Runtime, game resources or guest execution. It checks the 23 logged omissions plus Chinese, Japanese and Latin controls, verifying nonempty glyph ink and cell bounds:

| Configuration | Previously missing glyphs rendered | All probe samples |
| --- | ---: | ---: |
| Old default, Hiragino Sans W4 | 0 / 23 | 17 / 43 |
| Restored STHeiti selection | 23 / 23 | 43 / 43 |
| Prepared Baseline settings from corrected app | 23 / 23 | 43 / 43 |
| Prepared Candidate settings from corrected app | 23 / 23 | 43 / 43 |

Both corrected app copies passed strict signing and actual native-launcher preparation-only checks. Their game executable hashes match the originals. The corrected copies are under `out/testing/dist/font-fixed/`; the original apps and user records remain under their existing paths. The copy operation updated paths, shared font settings, effective configuration identities and bundle signatures. No game was started during this fix.

Local evidence: `out/testing/font-old-default.json`, `font-restored-heiti.json`, `font-baseline-prepared.json`, `font-candidate-prepared.json`, `font-pair-reconfiguration.json`, and `font-readiness.json`. Eleven packaging/readiness unit checks passed. The corrected apps retain the original observation/game build; later source-only recorder/menu changes are not included in this focused configuration repair.

## Limits and follow-up

The reported omissions and sample glyphs are verified through actual production rendering. Full live appearance and every possible game character have not been exhaustively checked. The user only needs to inspect game text in a corrected app; the four-case gameplay batch does not need to be repeated for this font check. The font setting changes the configuration identity, so old and corrected recordings must not be treated as identical starting configurations.
