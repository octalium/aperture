# aperture — project specification

## Goal

A personal raw photo processor built because existing FOSS options (darktable,
RawTherapee) feel wrong for the author's workflow. Single-developer scope.
Equal parts learning project and usable tool — the long-term test is whether
it replaces darktable for the author's own editing.

## Non-goals

- Feature parity with darktable, RawTherapee, or Lightroom
- Catalog / library / database (browse, rate, keyword, search)
- Broad camera support (v1 targets the author's camera only)
- Tethered shooting
- Print module
- Mobile, web, or cloud sync
- Multi-user

These are deliberately deferred — not "maybe later" hedges. Scope creep into
any of them resets the project's odds of shipping.

## Tech stack

- **Language:** C (C11 or newer)
- **Graphics + compute:** Vulkan (single context, shared between pipeline
  compute and viewport render)
- **UI:** Dear ImGui via `cimgui` C bindings, **`docking` branch** (gives
  dockable panels and multi-viewport — viewport can detach as its own OS
  window, e.g. fullscreen on a second monitor)
- **UI addons (optional, later):** `imnodes` if a node-graph view of the
  pipeline becomes desirable; `ImPlot` for histogram / waveform / vectorscope
- **Raw decode:** LibRaw
- **Lens corrections:** Lensfun
- **Color management:** LittleCMS (ICC) + OpenColorIO (working-space transforms)
- **Image I/O:** OpenImageIO (TIFF/JPEG/PNG export, EXIF preservation)
- **Build system:** TBD (CMake or Meson — pick at v0)

Rationale: leaning heavily on standard libraries for the parts where
reinventing the wheel is decades of work (raw decode, lens DBs, color
management). The novel work is the pipeline architecture, the Vulkan compute
graph, and the UI.

## Pipeline architecture

Stages, in order:

1. **Raw decode** (LibRaw) → linear sensor data + metadata
2. **Demosaic** — start with a known-good algorithm (AMaZE or RCD); X-Trans
   deferred until needed
3. **White balance** + **camera color profile** → working color space
   (scene-referred linear, likely Rec. 2020 or ACEScg)
4. **Lens corrections** (Lensfun) — distortion, vignette, chromatic aberration
5. **Tone mapping** — exposure, highlights, shadows, whites, blacks, contrast.
   Algorithm choice TBD (filmic, sigmoid, or curve); Lightroom's slider
   vocabulary is the user-facing contract regardless.
6. **Local edits** (v2+) — masks, gradients, brushes; per-mask tone adjustments
7. **Noise reduction + sharpening** (v3+)
8. **Output transform** → display or export color space
9. **Export** — JPEG/TIFF/PNG with embedded ICC profile and preserved EXIF

All stages run as Vulkan compute shaders against a tiled image graph. Edits
are non-destructive; pipeline state lives in an XMP sidecar (custom edit data
inside the standard XMP container — interop with other apps is not a goal).

The pipeline module is **UI-independent**. The ImGui shell is one frontend;
the same pipeline must be usable from a CLI batch processor.

## UI vision

"Debug menu over an image" aesthetic — floating panels, dense controls,
tool-style chrome. Reads as a power tool, not an unfinished consumer app.
Precedent: Nuke, Houdini, Substance Designer, Blender's editors.

- ImGui `docking` branch from day one
- Multi-viewport: image viewport can detach into its own OS window
- Image viewport is a Vulkan-rendered textured quad inside an ImGui window
- Module panels float; user composes their own layout

## Scope phases

### v0 — End-to-end skeleton

Goal: prove the whole stack works end-to-end with one image.

- Open one raw file from the author's camera
- LibRaw decode → simple bilinear demosaic → fixed WB → linear-to-sRGB
- ImGui shell with viewport and exposure / contrast / WB sliders
- JPEG export
- Vulkan pipeline runs on GPU (not a CPU prototype to be rewritten)

### v1 — Usable for actual editing

Goal: the author edits real photos with it.

- Full Lightroom-vocabulary tone tools
- AMaZE or RCD demosaic
- Camera color profile (DCP or ICC) for accurate color
- Lens corrections via Lensfun
- XMP sidecar persistence (load + save edits)
- TIFF/JPEG export with embedded ICC and preserved EXIF
- Histogram

### v2 — Local edits

- Radial and linear gradient masks
- Brush masking
- Per-mask tone adjustments
- Mask compositing (add / subtract / intersect)

### v3+ — Open

- Noise reduction, capture and output sharpening
- `imnodes` pipeline-as-graph view (if it earns its place)
- Multi-camera support (per-sensor color profiles)
- Whatever proves needed in actual use

## Key decisions and constraints

- **Single-camera at v1.** Color profile work is per-sensor. Generalize after
  the pipeline is proven on one body.
- **No catalog at v1.** Operate on a folder. Catalogs are a feature surface
  the project cannot afford early, and darktable's mandatory catalog is one of
  the things this project exists to avoid.
- **Pipeline is UI-independent.** UI shell is swappable. ImGui is the v1
  frontend, not a load-bearing decision.
- **Vulkan from the start, not a CPU prototype.** A CPU prototype would be
  rewritten anyway; the pipeline graph and tile management are the actual
  work.
- **Color science is the long pole.** Plumbing is fast with coding agents;
  taste-testing output is not. Budget time accordingly.
- **No workarounds.** If a stage produces wrong output, fix the stage. If the
  architecture forces special-casing in another stage, the architecture is
  wrong. (See user's global engineering standards.)

## Open questions

- Build system: CMake vs Meson
- Working color space: Rec. 2020 linear vs ACEScg
- Tone mapper: filmic vs sigmoid vs curve as the v1 default
- Sidecar schema: define before v1 ships so v0 prototypes don't lock in a bad
  shape
- Vulkan tile / memory management strategy for >40MP images at interactive
  rates
