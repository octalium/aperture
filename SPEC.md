# aperture — project specification

This document is the single source of truth for project scope. If a topic
isn't here, it isn't in scope. If a topic is in "deliberately deferred," it
isn't coming back without a deliberate decision.

## Goal

A personal raw photo processor built because existing FOSS options (darktable,
RawTherapee) feel wrong for the author's workflow. Single-developer scope.
Equal parts learning project and usable tool — the long-term test is whether
it replaces darktable for the author's own editing.

## Non-goals

- Feature parity with darktable, RawTherapee, or Lightroom
- Catalog / library / database
- Broad camera support (v1 targets the author's camera only)
- Interoperability with other apps' edit data, metadata sidecars, or catalog
  formats — see "Architectural stance" below
- Tethered shooting, print module, mobile, web, cloud sync, multi-user

## Architectural stance

Two principles drive every dependency and format decision in this spec:

1. **Build the best solution, not on the shoulders of false giants.** Use a
   third-party library when it solves a problem we genuinely cannot solve
   ourselves (raw decoding, ICC math, lens databases). Drop wrappers, config
   frameworks, and "convenience" layers — they bring surface area without
   carrying their weight.
2. **No external interop.** Aperture defines its own sidecar format and reads
   only the metadata that the source file's container specifies (EXIF in raw
   files, etc.). It does not read XMP sidecars from Lightroom or darktable,
   does not write XMP for them to consume, and does not pretend its data is
   portable. If a user wants migration, a third-party converter is the right
   abstraction — not a bidirectional compatibility layer baked into the core.

## Tech stack

### Language and runtime

- **C** (C11 or newer)
- **Vulkan** for compute and viewport rendering (single context shared)
- **Build:** Meson — chosen for cross-compilation ergonomics and clean
  vendored-dependency management

### Dependencies kept

| Library | Job | Why kept |
|---|---|---|
| **LibRaw** | Raw decode + EXIF read | Decades of per-camera reverse engineering; not reproducible |
| **Lensfun** | Lens corrections | The lens correction *database* is the value; can't be recreated |
| **LittleCMS** | ICC profile math | ICC color management is non-trivial math against a real format |
| **libjpeg-turbo** | JPEG encode/decode | Real giant; format is non-trivial |
| **libtiff** | TIFF encode/decode | Real giant; handles 8/16/32-bit, ICC embedding, EXIF as IFD |
| **libpng** | PNG encode/decode | Real giant |
| **cimgui** (ImGui `docking` branch) | UI shell | Deliberate aesthetic + functional fit |
| **Vulkan SDK / volk / VMA** | Graphics API + helpers | The platform |

### Dependencies deliberately not used

| Library | Job it was for | Why dropped |
|---|---|---|
| OpenColorIO | Color-space transform config | A config wrapper aimed at VFX; our compute shaders apply working-space transforms directly |
| OpenImageIO | Image I/O abstraction | A wrapper over libjpeg/libtiff/libpng; we use those directly |
| Exiv2 | EXIF/IPTC/XMP read/write | Brings C++ runtime and XMP semantics; we don't need XMP, EXIF read comes from LibRaw, EXIF write is bounded |
| Expat / libxml2 | GPX parsing | GPX subset we care about is small enough to parse directly |

### UI addons (later, optional)

- `imnodes` if a node-graph view of the pipeline becomes desirable
- `ImPlot` for histogram / waveform / vectorscope rendering

## Pipeline architecture

Stages, in order:

1. **Raw decode** (LibRaw) → linear sensor data + metadata
2. **Demosaic** — start with a known-good algorithm (AMaZE or RCD); X-Trans
   deferred until needed
3. **White balance** + **camera color profile** → working color space
4. **Lens corrections** (Lensfun) — distortion, vignette, chromatic aberration
5. **Geometric edits** — crop, rotate, straighten, perspective correction
   (single resampling step downstream of lens corrections, so the
   undistorted image is what gets cropped)
6. **Tone mapping** — exposure, highlights, shadows, whites, blacks, contrast
7. **Local edits** (v2+) — masks, gradients, brushes; per-mask tone adjustments
8. **Noise reduction + sharpening** (v3+)
9. **Output transform** → display or export color space
10. **Export** — JPEG / TIFF / PNG with embedded ICC profile and authored EXIF

All stages run as Vulkan compute shaders against a tiled image graph. Edits
are non-destructive; pipeline state lives in the `.aperture` sidecar (see
edit state model).

The pipeline module is **UI-independent**. The ImGui shell is one frontend;
the same pipeline must be usable from a CLI batch processor.

### Working color space

Default: **Rec. 2020 linear**. Stored as a configurable runtime parameter
from day one — *not* a hardcoded constant. The pipeline accepts a working
space identifier and the corresponding transform matrices; swapping to
ACEScg or any other wide-gamut linear space is a configuration change, not
a code change. ACES integration is *not* a v1 deliverable; the seam to add
it cheaply later is.

### Tone mapping

Default: **sigmoid**. Filmic and traditional tone curve must also be
selectable per-image. The tone-mapping stage is a dispatchable family —
pipeline state names which mapper to use, and the mapper is fully
parameterized in the sidecar. Default does not constrain user choice.

### Geometric edits

- Crop with aspect-ratio constraints (free, original, common ratios, custom)
- Rotate in 90° increments (lossless reorientation of the canvas)
- Straighten with a horizon-line tool
- Perspective correction — manual four-point or guided vertical/horizontal
- Resample at export only; previews use the GPU's bilinear/bicubic in the
  viewport shader

## Metadata & tagging

Metadata that aperture authors lives in the `.aperture` sidecar. Metadata
that the source file already carries (camera, exposure, lens, capture time,
GPS if present) is read once via LibRaw on import and copied into the
sidecar's "captured" section. The source raw is never modified.

### Read paths

- **EXIF on import** — LibRaw exposes the dictionary; we copy what we care
  about into the sidecar.
- **No external XMP**. Aperture does not read `.xmp`, `.pp3`, `.cof`, or any
  other tool's sidecar. Migration tools are a third-party concern.

### Write paths

- **`.aperture` sidecar** — the canonical record of all aperture-authored
  metadata and edit state.
- **Embedded EXIF on export** — JPEG/TIFF/PNG outputs carry EXIF (camera,
  exposure, lens, capture time, GPS coordinates aperture authored via GPX
  geotagging or manual entry). Implementation: small in-house EXIF/TIFF-IFD
  writer (~few hundred lines, isolated in `src/metadata/exif_writer.c`).
  No external EXIF library.
- **Embedded ICC on export** — via LittleCMS through the libjpeg-turbo /
  libtiff / libpng metadata APIs.

### Authored metadata fields

All stored in the `.aperture` sidecar:

- **Rating** — 0–5 stars
- **Color label** — red / yellow / green / blue / purple
- **Pick / reject flag**
- **Keywords** — flat and **hierarchical**. Default authoring separator
  `|`; configurable. On read, separator is detected per-keyword.
- **Title**, **caption**, **headline**
- **Creator**, **copyright**, **rights**, **contact** — settable as
  per-import or per-camera defaults
- **Location** — city, state, country (textual) plus GPS coordinates
- **Capture time + originating timezone** — UTC + IANA zone alongside the
  raw EXIF timestamp

Search/filter UI over these fields is a v3+ concern; until then, fields
are authored and persisted but discovery relies on the filesystem.

### Geotagging via GPX

Workflow: author carries a GPS recorder while shooting; aperture matches
photo capture timestamps against the GPX track and writes interpolated
GPS coordinates into the sidecar (and into exported file EXIF).

**GPX format.** Small XML schema. The relevant subset:

```
gpx > trk > trkseg > trkpt[@lat, @lon] > ele, time
```

Multiple tracks per file, multiple segments per track, gaps between
segments treated as "no signal."

**Parser.** In-house. ~150 lines of C. The schema is small enough that
pulling in a general XML library is unjustified.

**Timestamp interpolation.**

- Linear interpolation between adjacent `trkpt` timestamps for lat/lon/ele
- Camera capture time comes from EXIF `DateTimeOriginal` /
  `SubSecTimeOriginal`
- A photo whose timestamp falls inside a segment gets interpolated coords
- A photo near (within configurable threshold, default 60s) but outside a
  segment can be snapped to the nearest endpoint or refused — user setting
- A photo far from any segment is left untagged with a warning

**Clock offset reconciliation.** Camera clocks drift. The user provides a
fixed offset (e.g. "camera is +47s vs GPS UTC") that aperture applies to
all EXIF timestamps before interpolation. Helper UI: shoot a photo of the
GPS display; aperture computes the offset from the visible time vs the
photo's EXIF time.

**Timezone handling.** EXIF `DateTimeOriginal` is naive (no timezone). GPX
times are UTC. The user provides the camera's timezone (default: system
timezone at import); aperture stores both the original timestamp and
resolved UTC in the sidecar.

**Batch workflow.** Load one or more GPX files into a session. Select
photos. Apply geotag. Diff preview before write (which photos match,
which don't, which are at endpoints). Commit writes coordinates to the
`.aperture` sidecar; export carries them into output EXIF.

**Existing GPS data.** If a photo already has GPS coords in source EXIF,
aperture preserves them by default. Overwrite is opt-in per session.

## Edit state model

Non-destructive editing is the foundation. Every edit is a stack of
parameters; the source raw is never modified.

### The `.aperture` sidecar

- **One file per source image**, sitting alongside it: `IMG_0001.RAF` →
  `IMG_0001.RAF.aperture`
- **TOML format.** Diffable, human-readable, well-tooled, schema-friendly
- **Top-level schema version field.** Migrations between schema versions
  are explicit code paths, not best-effort guessing
- **Sections** (proposed; final shape designed before v1):
  - `[aperture]` — schema version, aperture version that wrote it
  - `[source]` — path/hash to source raw, captured EXIF dictionary
  - `[metadata]` — authored ratings, keywords, captions, location, etc.
  - `[edit]` — pipeline parameters per virtual copy
  - `[history]` — ordered list of significant edits, with rollback targets
  - `[snapshots]` — named full-state save points
  - `[copies]` — virtual copies (independent edit stacks against same source)

### Behaviors

- **Undo / redo** — in-memory ring buffer per session; not persisted
- **History** — persistent ordered log of significant edits (parameter-level
  noise is filtered out); user can roll back to any history entry
- **Snapshots** — named save-points of the full edit state, persisted in
  the sidecar
- **Virtual copies** — multiple independent edit stacks against the same
  source raw, persisted in the sidecar as separate entries

### Schema commitment

The `.aperture` schema is **frozen at v1**. v0 may iterate freely. Once
v1 ships, schema changes go through versioned migrations with explicit
upgrade code. Schema design gets its own design doc when we approach v1.

## UI vision

"Debug menu over an image" aesthetic — floating panels, dense controls,
tool-style chrome. Reads as a power tool, not an unfinished consumer app.
Precedent: Nuke, Houdini, Substance Designer, Blender's editors.

- ImGui `docking` branch from day one
- Multi-viewport: image viewport can detach into its own OS window
- Image viewport is a Vulkan-rendered textured quad inside an ImGui window
- Module panels float; user composes their own layout
- Keyboard-first where it makes sense (rating, flagging, navigation between
  images); mouse-driven for sliders and masks

## Scopes & display tools

- **Histogram** — RGB and luminance, log/linear toggle, configurable scope
  (full image / crop / mask)
- **Waveform** — luminance and RGB parade
- **Vectorscope** — chrominance distribution
- **Before/after** — split view and full-toggle
- **Soft proofing** — preview with output ICC profile and gamut warning
- **Pixel readout** — sample any point's pre/post-pipeline values

## File format support

### Input

- **Raw** — whatever LibRaw decodes for the author's camera at v1; broader
  list at v3+
- **DNG** — first-class input from v0 (LibRaw handles it)
- **JPEG / TIFF / PNG** — for editing already-processed images and for
  sidecar testing; same pipeline runs, demosaic stage is bypassed

### Output

- **JPEG** — 8-bit, embedded ICC, embedded EXIF, configurable quality
- **TIFF** — 8 / 16 / 32-bit, optional compression, embedded ICC and EXIF
- **PNG** — 8 / 16-bit, embedded ICC and EXIF

### Sidecar

- **`.aperture`** — one per source file, TOML, aperture's own schema

## Performance targets

- Slider drag latency: visible-region update under **16ms** at 4K viewport
  on the author's hardware (interactive feel)
- Full-image re-render on parameter change: under **200ms** for a 45MP raw
  on the author's GPU
- Cold open of a raw file: under **2s** including LibRaw decode
- Idle CPU/GPU when not editing: effectively zero

These are targets, not contracts. Missing them by 50% on pathological
inputs is acceptable; missing them on common cases is a bug.

## Platform support

- **Linux (primary).** Author's daily driver. Wayland-first.
- **Windows / macOS** — not v1. Vulkan + ImGui + chosen libs are all
  portable; porting is mechanical when motivated.

## Scope phases

### v0 — End-to-end skeleton

Goal: prove the whole stack works end-to-end with one image.

- Open one raw file from the author's camera
- LibRaw decode → simple bilinear demosaic → fixed WB → linear-to-sRGB
- ImGui shell with viewport and exposure / contrast / WB sliders
- JPEG export with EXIF passthrough
- Vulkan pipeline runs on GPU (not a CPU prototype to be rewritten)

### v1 — Usable for actual editing

Goal: the author edits real photos with it.

- Full Lightroom-vocabulary tone tools
- AMaZE or RCD demosaic
- Camera color profile (DCP or ICC) for accurate color
- Lens corrections via Lensfun
- Geometric edits (crop, rotate, straighten)
- Histogram + before/after
- `.aperture` sidecar persistence with frozen schema
- Metadata read on import (EXIF via LibRaw)
- TIFF/JPEG/PNG export with embedded ICC and authored EXIF

### v1.5 — Metadata authoring

- Ratings, color labels, pick/reject flags
- Keywords (flat + hierarchical)
- Captions, titles, copyright
- Per-import authorship defaults
- GPX-based geotagging (full workflow: load tracks, offset reconciliation,
  batch apply, EXIF GPS write on export)

### v2 — Local edits

- Radial and linear gradient masks
- Brush masking
- Per-mask tone adjustments
- Mask compositing (add / subtract / intersect)
- Perspective correction tool

### v3+ — Open

- Noise reduction, capture and output sharpening
- Waveform / vectorscope / soft proofing
- `imnodes` pipeline-as-graph view (if it earns its place)
- Multi-camera support (per-sensor color profiles)
- Search / filter UI over metadata
- Snapshots and virtual copies UI
- Whatever proves needed in actual use

## Deliberately deferred

- Catalog / library database
- Reading or writing other apps' sidecars (XMP, .pp3, .cof)
- AI subject / sky detection
- Tethered shooting
- Print module
- Web / mobile / cloud sync
- Face detection
- Multi-user

## Key decisions and constraints

- **No false giants.** Every dependency must be the *right tool*, not a
  convenience wrapper. Wrappers (OCIO, OIIO) are dropped; format-specific
  giants (libjpeg-turbo, libtiff, libpng, LibRaw, Lensfun, LittleCMS) are
  kept.
- **No external interop.** Aperture defines its own sidecar and ignores
  third-party edit data and metadata sidecars. Migration is a third-party
  concern.
- **Single-camera at v1.** Color profile work is per-sensor. Generalize
  after the pipeline is proven on one body.
- **No catalog.** The filesystem is the catalog. Metadata is authored and
  persisted in the `.aperture` sidecar.
- **Pipeline is UI-independent.** UI shell is swappable. ImGui is the v1
  frontend, not a load-bearing decision.
- **Vulkan from the start, not a CPU prototype.** A CPU prototype would be
  rewritten anyway; the pipeline graph and tile management are the actual
  work.
- **Color science is the long pole.** Plumbing is fast; taste-testing
  output is not. Budget time accordingly.
- **No workarounds.** If a stage produces wrong output, fix the stage. If
  the architecture forces special-casing in another stage, the architecture
  is wrong.

## Open questions

- **`.aperture` TOML schema** — final shape designed before v1 ships. v0
  iterates freely.
- **Working color space configuration interface** — the parameter exists
  from day one; the exact API for swapping it (config file, runtime
  setting, both) is a v1 design call.
- **DCP vs ICC** for camera color profiles — both are well-defined; pick
  one (or support both) when v1 color profile work begins.
- **Tone-mapper coefficient defaults** — sigmoid is the default mapper;
  the default coefficients (curve shape) need taste-testing on real images
  before v1.
