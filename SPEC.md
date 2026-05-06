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
- Catalog / library / database (browse, rate, keyword, search at scale) — see
  metadata section for what *is* in scope
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
- **Image I/O (export):** OpenImageIO
- **Metadata read/write:** TBD — likely Exiv2 with a thin C++ shim (covers
  EXIF + IPTC + XMP in one library); fallback path is libexif + libiptcdata +
  Exempi
- **GPX parsing:** TBD — likely Expat (small streaming XML in C); GPX schema
  is simple enough to roll our own if a dependency feels heavy
- **Build system:** TBD (CMake or Meson — pick at v0)

Rationale: leaning heavily on standard libraries for parts where reinventing
is decades of work (raw decode, lens DBs, color management, metadata
serialization). The novel work is the pipeline architecture, the Vulkan
compute graph, the edit state model, and the UI.

## Pipeline architecture

Stages, in order:

1. **Raw decode** (LibRaw) → linear sensor data + metadata
2. **Demosaic** — start with a known-good algorithm (AMaZE or RCD); X-Trans
   deferred until needed
3. **White balance** + **camera color profile** → working color space
   (scene-referred linear, likely Rec. 2020 or ACEScg)
4. **Lens corrections** (Lensfun) — distortion, vignette, chromatic aberration
5. **Geometric edits** — crop, rotate, straighten, perspective correction
   (applied as a single resampling step downstream of lens corrections so the
   undistorted image is what gets cropped)
6. **Tone mapping** — exposure, highlights, shadows, whites, blacks, contrast.
   Algorithm choice TBD (filmic, sigmoid, or curve); Lightroom's slider
   vocabulary is the user-facing contract regardless.
7. **Local edits** (v2+) — masks, gradients, brushes; per-mask tone adjustments
8. **Noise reduction + sharpening** (v3+)
9. **Output transform** → display or export color space
10. **Export** — JPEG / TIFF / PNG with embedded ICC profile and full
    metadata round-trip (see metadata section)

All stages run as Vulkan compute shaders against a tiled image graph. Edits
are non-destructive; pipeline state lives in a sidecar (see edit state model).

The pipeline module is **UI-independent**. The ImGui shell is one frontend;
the same pipeline must be usable from a CLI batch processor.

## Geometric edits

Treated as a pipeline stage rather than a UI gimmick because crops affect
histogram, scopes, and any local-edit mask coordinates downstream.

- **Crop** with aspect-ratio constraints (free, original, common ratios,
  custom)
- **Rotate** in 90° increments (lossless reorientation of the canvas)
- **Straighten** with a horizon line tool
- **Perspective correction** — manual four-point or guided vertical/horizontal
- **Resample** at export only; previews use the GPU's bilinear/bicubic in the
  viewport shader

## Metadata & tagging

A first-class concern, not an afterthought. Photographs without metadata are
unsearchable, untimestamped, and rights-ambiguous. Aperture writes metadata
that other tools (and the author's future self) can read without aperture
present.

### Standards

- **EXIF** — camera/exposure data, GPS, orientation, capture time. Read from
  raw, preserved on export, written on geotag.
- **IPTC Core / Extension** — captions, headlines, keywords, copyright,
  creator, contact info, location names. The interoperability format for
  authorship and descriptive metadata.
- **XMP** — Adobe's RDF/XML container; carries IPTC fields plus tool-specific
  edit data. Used both embedded (in JPEG/TIFF/DNG) and as sidecar (`.xmp`
  next to `.raw`).
- **Maker notes** — preserved on round-trip, not interpreted.

Edit state lives in **aperture's own XMP namespace** inside the standard XMP
container. Interop with other apps' edit data is explicitly not a goal —
aperture-to-aperture round-trip is.

### Library / catalog metadata (without a catalog)

The author's stance: a catalog database is the wrong abstraction. Filesystem
folders are the catalog. But the *metadata* a catalog would track is still
useful — it just lives in the file or sidecar, not a database.

Supported per-image:

- **Rating** — 0–5 stars (`xmp:Rating`)
- **Color label** — red / yellow / green / blue / purple (`xmp:Label`)
- **Pick / reject flag** — Lightroom-compatible (`lr:pick` extension)
- **Keywords** — flat and **hierarchical** (e.g. `Animals|Birds|Sparrow`);
  standard XMP/IPTC encoding so Lightroom, digiKam, and others see them
- **Title**, **caption/description**, **headline**
- **Creator / copyright / rights** — IPTC Core fields, settable as
  per-import or per-image defaults
- **Location** — IPTC location fields (city, state, country) plus EXIF GPS

Search/filter UI over these fields is a v2+ concern. Until then, metadata is
authored and persisted; finding things relies on the filesystem and external
tools.

### Geotagging via GPX

Workflow: author carries a GPS recorder (handheld, watch, phone) while
shooting. The recorder produces a GPX track. Aperture matches each photo's
capture timestamp against the track and writes interpolated GPS coordinates
to the photo's EXIF GPS tags.

**GPX format.** GPX is a small, well-defined XML schema. The relevant subset:

```
gpx > trk > trkseg > trkpt[@lat, @lon] > ele, time
```

Multiple tracks per file, multiple segments per track, gaps between segments
treated as "no signal."

**Timestamp interpolation.**

- Linear interpolation between adjacent `trkpt` timestamps for lat/lon/ele
- Camera capture time comes from EXIF `DateTimeOriginal` / `SubSecTimeOriginal`
- A photo whose timestamp falls inside a segment gets interpolated coords
- A photo near (within configurable threshold, default 60s) but outside a
  segment can be snapped to the nearest endpoint or refused — user setting
- A photo far from any segment is left untagged with a warning

**Clock offset reconciliation.** Camera clocks drift. The user provides a
fixed offset (e.g. "camera is +47s vs GPS UTC") that aperture applies to all
EXIF timestamps before interpolation. Helper UI: shoot a photo of the GPS
display, aperture computes the offset from the visible time vs the photo's
EXIF time.

**Timezone handling.** EXIF `DateTimeOriginal` is naive (no timezone). GPX
times are UTC. The user provides the camera's timezone (default: system
timezone at import); aperture converts to UTC for matching. v1 stores
timezone alongside edits in the sidecar.

**Batch workflow.** Load one or more GPX files into a session. Select
photos. Apply geotag. Diff preview before write (which photos match, which
don't, which are at endpoints). Commit writes to EXIF and to sidecar.

**Existing GPS data.** If a photo already has GPS coords (e.g. shot on a
phone), aperture preserves them by default. Overwrite is opt-in per session.

### Authorship & rights

- Per-import defaults (creator, copyright, contact, usage terms) configurable
  per-camera or per-folder profile
- Applied to IPTC fields automatically on import; user can override per-image
- Survives export round-trip

### Timezone handling

- All internal timestamps stored as UTC + originating timezone
- Display in user's local zone or shooting zone, configurable
- GPX matching always done in UTC

## Edit state model

Non-destructive editing is the foundation. Every edit is a stack of
parameters; the source raw is never modified.

- **Pipeline state** — every stage's parameters, serialized as JSON or RDF
  inside the XMP sidecar under aperture's namespace
- **Undo/redo** — in-memory ring buffer per session; not persisted
- **History** — persistent ordered log of significant edits (param-level
  history is too noisy); user can roll back to any history entry
- **Snapshots** — named save-points of the full edit state, persisted in the
  sidecar; cheap because they're parameter sets, not pixels
- **Virtual copies** — multiple independent edit stacks pointing at the same
  source raw, persisted in the sidecar as separate entries
- **Sidecar layout** — one `.xmp` per source file, sitting next to it.
  Standard XMP container so other tools see metadata; aperture's namespace
  carries edit data they ignore.

The sidecar **schema** is defined and committed before v1 ships. v0 may
prototype freely; v1 freezes the schema and any future changes are migrations.

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

- **JPEG** — 8-bit, embedded ICC, embedded EXIF/IPTC/XMP, configurable quality
- **TIFF** — 8 / 16 / 32-bit, optional compression, embedded ICC and metadata
- **PNG** — 8 / 16-bit, embedded ICC and metadata

### Sidecar

- **`.xmp`** — one per source file, RDF/XML, standard XMP namespaces plus
  aperture's namespace for edit data

## Performance targets

- Slider drag latency: visible-region update under **16ms** at 4K viewport
  on the author's hardware (interactive feel)
- Full-image re-render on parameter change: under **200ms** for a 45MP raw
  on the author's GPU
- Cold open of a raw file: under **2s** including LibRaw decode
- Idle CPU/GPU when not editing: effectively zero

These are targets, not contracts. Missing them by 50% on pathological inputs
is acceptable; missing them on common cases is a bug.

## Platform support

- **Linux (primary).** Author's daily driver. Wayland-first, X11 by accident
  via SDL/GLFW.
- **Windows / macOS** — not v1. Vulkan + ImGui + chosen libs are all portable;
  porting is mechanical when motivated.

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
- XMP sidecar persistence with frozen schema
- Metadata read on import (EXIF, existing IPTC/XMP)
- TIFF/JPEG/PNG export with embedded ICC, EXIF, IPTC, XMP

### v1.5 — Metadata authoring

- Ratings, color labels, pick/reject flags
- Keywords (flat + hierarchical)
- Captions, titles, copyright
- Per-import authorship defaults
- GPX-based geotagging (full workflow: load tracks, offset reconciliation,
  batch apply, EXIF GPS write)

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
- AI subject / sky detection
- Tethered shooting
- Print module
- Web / mobile / cloud sync
- Face detection
- Multi-user

## Key decisions and constraints

- **Single-camera at v1.** Color profile work is per-sensor. Generalize after
  the pipeline is proven on one body.
- **No catalog at v1 (or ever, probably).** Operate on a folder. The
  filesystem is the catalog. Metadata is authored and persisted in standard
  formats so external catalogs can index aperture's output if anyone wants.
- **Pipeline is UI-independent.** UI shell is swappable. ImGui is the v1
  frontend, not a load-bearing decision.
- **Vulkan from the start, not a CPU prototype.** A CPU prototype would be
  rewritten anyway; the pipeline graph and tile management are the actual
  work.
- **Color science is the long pole.** Plumbing is fast with coding agents;
  taste-testing output is not. Budget time accordingly.
- **Metadata is interoperable; edit data is not.** Aperture writes standard
  EXIF/IPTC/XMP that any tool can read. Aperture's edit parameters live in
  aperture's namespace and are not expected to round-trip with Lightroom.
- **No workarounds.** If a stage produces wrong output, fix the stage. If the
  architecture forces special-casing in another stage, the architecture is
  wrong. (See user's global engineering standards.)

## Open questions

- **Build system:** CMake vs Meson
- **Working color space:** Rec. 2020 linear vs ACEScg
- **Tone mapper default:** filmic vs sigmoid vs curve
- **Sidecar schema:** define before v1 ships; v0 may iterate
- **Vulkan tile / memory management** strategy for >40MP images at
  interactive rates
- **Metadata library choice:** Exiv2 (C++ shim cost) vs libexif + libiptcdata
  + Exempi (three deps, all C, more surface area)
- **GPX parser choice:** Expat vs roll-our-own (schema is small enough that
  the second is reasonable)
- **Hierarchical keyword separator:** `|` (Lightroom) vs `/` (digiKam) — pick
  one for our authoring UI; on read, accept both
