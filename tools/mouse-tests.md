# Mouse pixel-perfect test cases

Reproducible tests for the mcpbridge closed-loop mouse. Reusable harness:
`tools/mouse-tests.ps1` (copied to `out/mousetest.ps1` during development).

## How the closed loop works (why it is mode-independent)

`mouse_move {x,y}` targets **Intuition screen pixels** (the same space
`get_pointer_pos` reports). Each call:
1. reads the live pointer position via `mcpbridge_get_pointer_pos`
   (IntuitionBase MouseX/MouseY),
2. issues a relative mouse-counter delta toward the target (chunked ≤40
   counts per drain tick to avoid 8-bit MOUSE0DAT aliasing),
3. re-reads after a ~30 ms settle and repeats (≤10 passes), stopping when
   within 1 px or when it stops improving.

Because it is a feedback loop on Intuition's own coordinate, accuracy does
not depend on host window scaling or the Amiga display mode — only on
`get_pointer_pos` being valid in that mode (it is, for every Intuition
screen, native or RTG). The only theoretical degradation is in very coarse
modes where one mouse count maps to >1 screen pixel; there the loop settles
within one step (bounded ≤ a couple px) instead of exact 0.

## Setup

```powershell
out\winuae64.exe -f vm\mcamiga-aga-local.uae   # AGA, AmigaOS 3.2, 640x512 hires-laced
# wait for Workbench, then connect a JSON-RPC client to 127.0.0.1:7843
```

Each test: `mouse_move {x,y}` then `get_pointer_pos`, assert
`|got-target| <= 1px`. A sweep covers corners, centre, and edges.

## Results (AGA, AmigaOS 3.2, 640x512 hires-laced)

| # | Test | Method | Result |
|---|---|---|---|
| M1 | Small window | `set_config gfx_fullscreen_amiga=false`, sweep 6 targets | **0 px** |
| M2 | Full window (maximized) | `gfx_fullscreen_amiga=fullwindow`, sweep 6 targets | **0 px** |
| M3 | Exclusive fullscreen | `gfx_fullscreen_amiga=true`, sweep 6 targets | **0 px** |
| M4 | Full-span sweep | corners (0,0)/(638,510), centre, mid-edges | **0 px** |
| M5 | Edge clamping | `mouse_move 4000,4000` → reads (638,510) | correct clamp to screen bounds |
| M6 | Persistence | after `reset hard` + reboot, repeat M1 sweep | **0 px** |
| M7 | Functional double-click | `mouse_move` to Workbench icon + `mouse_click count:2` | window opens (icon hit exactly) |

Target sets used (640x512): `(20,20) (100,40) (300,80) (500,150) (620,250)
(37,133)`; full-span: `(0,0) (160,128) (319,255) (478,382) (638,510) (574,5)
(5,459)`.

## Verified separately (mouse-only Workbench navigation)

Driving Prefs → ScreenMode entirely by mouse (documented earlier in
MCPBRIDGE_PROGRESS.md) confirmed multi-window navigation: the pointer landed
dead-on the Prefs drawer icon and gadgets across nested windows. Every
`mouse_move` reached its commanded Intuition coordinate; the only inaccuracy
in those flows was the *test harness's* estimate of an icon's coordinate from
a screenshot (overscan offset ≈ (54,28)), not the mouse.

## Not completed in automated testing

- **Different native resolutions (Lores 320x256 / Super-High Res 1280x256):**
  switching the Workbench screenmode requires driving the ScreenMode prefs
  "Use" + "untested mode" + "close windows" requester chain, which did not
  complete reliably under automation (a GUI-choreography limitation — the
  mouse itself hit every commanded coordinate; keyboard list-selection via
  click-to-focus + Up×N/Down×N is reliable, but the apply/confirm requesters
  tangled the window state). The closed-loop guarantee above applies to these
  modes; the only expected difference is a possible ≤1-2px settle in
  Super-High Res due to coarser per-count granularity.
- **RTG (Picasso96 / uaegfx):** Workbench on an RTG screen requires a uaegfx
  screenmode, which was not present in the visible PAL ScreenMode list (needs
  the uaegfx monitor active + list scrolling). `get_pointer_pos` reads
  IntuitionBase MouseX/MouseY which Intuition maintains for RTG screens too,
  so the closed loop is expected to be pixel-perfect there by the same
  mechanism; this was not empirically confirmed.

To extend coverage later: get Workbench onto the target screenmode by hand
(or a saved config that boots into it), then run `tools/mouse-tests.ps1`,
which auto-detects screen bounds via the overshoot-clamp probe and sweeps
relative to them — so it works at any resolution without code changes.
