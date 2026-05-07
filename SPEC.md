# aperture — project specification

This document is the single source of truth for project scope. If a topic
isn't here, it isn't in scope. If a topic is in "deliberately deferred,"
it isn't coming back without a deliberate decision.

## Goal

aperture is a raw photo **library**, **culling**, **grading**, and
**export** tool. It manages a photographer's working bank of raw images,
organizes them into user-defined groups, applies non-destructive edits
through reusable pipelines, and produces final JPEG / TIFF / PNG outputs.

The intended workflow:

1. The photographer **imports** raws into the aperture library.
2. They **browse, rate, group, and cull** in Library mode.
3. For keepers they apply **pipelines** (reusable edit presets) and
   **per-photo edits** — exposure, color, tone, lens corrections,
   metadata.
4. They **export** finished files for delivery. Anything that needs
   pixel-level retouching goes to Photoshop / Affinity / specialist
   tools afterward.

**Aperture's job stops at "well-organized, exposure-and-color-correct,
exported."** It is deliberately not a full photo editor.

## Non-goals

- Pixel-level retouching: heal, clone, dodge, burn, liquify, frequency
  separation, etc.
- Layers, compositing, complex masks (a mask for crop or a region of
  interest may exist; layered painting does not)
- Vector tools, text, drawing
- Stitching, panoramas, focus stacking
- Time-lapse and video
- Catalog spanning multiple libraries
- Cloud sync, web, mobile apps
- Multi-user, collaboration
- Print module
- Tethered shooting

## Architectural stance

Three principles drive every dependency, format, and structural decision.

1. **Build the best solution, not on the shoulders of false giants.**
   Use third-party libraries where reinventing is decades of work
   (raw decode, color profiles, JPEG / TIFF / PNG, GPX, SQLite).
   Drop wrappers and convenience layers that don't carry their weight
   (OCIO, OIIO, Exiv2, etc.).

2. **No external interop.** Aperture defines its own sidecar (TOML
   `.aperture`) and library (SQLite) formats. Migration to/from
   Lightroom / darktable / Capture One is a third-party concern, not
   a bidirectional compatibility layer in the core.

3. **Module registry as a first-class architectural concept.** Every
   functional unit — input format readers, demosaic, color, tone,
   geometric corrections, AI denoise / classification, output
   encoders, metadata writers — is a self-contained module that
   registers with a typed slot. Adding a feature is "drop in a file
   + add to the registry," not a core rewrite. The realistic test:
   adding AI denoise should be a one-file change, not an application
   restructure.

## Domain model

### Library

A user-chosen root directory containing photos and a single SQLite
index (`<library>/library.aperture-db`). Subfolder structure inside
the library is the user's business — aperture treats the filesystem
as the photographer's organization layer.

The library db holds:

- Photo index: path, content hash, capture timestamp, basic metadata
  cached for fast filtering
- User-defined groups + group membership (groups are
  application-level state, not per-photo state)
- Pipeline definitions (named, versioned)
- Library-level settings: import naming schemas, export presets

The db is a **derived index over the filesystem and per-photo
sidecars** — re-buildable by re-scanning. Sidecars + filesystem are
authoritative for per-photo state. Only group membership and pipeline
definitions live exclusively in the db.

### Photo

A single source file in the library, accompanied by a `.aperture`
sidecar (TOML). Most photos are raws (Bayer cameras at v1); JPEG
and TIFF are valid sources.

The sidecar carries:

- The photo's edit stack (see Edit model)
- Per-photo metadata (rating, color label, flags, keywords,
  captions, GPS, copyright)

Sidecars are portable — they move with the file. A sidecar without
the source file is a stub; a source without a sidecar is unedited
(aperture creates one on first edit).

### Group

A user-defined collection of photos. A photo can belong to multiple
groups. Membership lives in the library db, not in sidecars — groups
are application-level (cross-photo) state, and finding "all photos
in group X" should be a db query, not a filesystem scan.

### Edit / Pipeline

Aperture's non-destructive edit model has two construct kinds:

- **Edit** — a single instance of a module with parameters. Example:
  `Exposure { ev = +0.5 }` or `LensCorrection { auto = true }`.
- **Pipeline** — a named, reusable, ordered sequence of edits. Example:
  `"Portrait Look v3" = [WhiteBalance{...}, Exposure{...}, Tone{...}]`.

A photo's edit state is a **stack** — an ordered list of items, each
of which is either a direct Edit or a Pipeline reference. The user
can drag-and-drop to reorder the stack. At render time the stack is
expanded: pipeline references flatten into their constituent edits,
and the resulting linear chain runs through the GPU pipeline graph.

**Pipeline references are live.** When the user edits a pipeline
definition, every photo whose stack references that pipeline picks up
the change immediately. There is no per-photo snapshotting at apply
time. (Capture One's model, not Lightroom's.)

If a photographer wants to "freeze" a pipeline at a particular state,
they can:

- Duplicate the pipeline as a new ID (manual snapshot)
- Or expand the pipeline reference into its constituent edits in the
  photo's stack (a future feature)

### Export

A photo + export settings → output file. Settings include format
(JPEG / TIFF / PNG), bit depth, quality / compression, ICC profile,
metadata-included flags, naming schema, and destination.

Export targets are flexible:

- Side-by-side in the library (`IMG_0001.NEF` → `IMG_0001.jpg`)
- An external directory chosen by the user

The library db stores **export presets** (web JPEG, print TIFF,
archive TIFF, etc.). A preset is a named bundle of settings.

## Application modes

The canvas is the primary display area. Its content — and the panels
visible alongside it — depend on the current mode.

| Mode | Canvas | Contextual panels |
|---|---|---|
| **Library** | Browseable grid of photo thumbnails | Filter, Groups, Import, Library settings |
| **Photo** | Single photo with pan / zoom | Edit modules (per-photo or pipeline-editing), Histogram, Metadata |
| **Export** | Preview of selected photos + queue | Format, Quality, Naming, Destination |

Mode transitions are deliberately kept dynamic — opening a single
photo from a thumbnail will be a click; opening multiple for batch
edits should be possible; specific bindings are not load-bearing
until each mode has substance.

Modules declare which mode(s) their UI participates in. An exposure
slider is Photo-mode; an "import naming schema" UI is Library-mode.
The mode shell renders the right set automatically.

## Module registry

### Why this is the architecture

Every feature aperture grows is a module. Decode-NEF, Demosaic,
WhiteBalance, Exposure, Tone, LensCorrection, AIDenoise,
JPEGExport, EXIFWriter, GPXGeotag — same shape, same registration
mechanism. The core engine doesn't know about specific modules; it
walks the registry.

This is what lets adding AI denoise be a one-file change.

### Module categories

- **Input** — file format readers (LibRaw, JPEG, TIFF)
- **Color science** — demosaic, white balance, color profile,
  working-space transforms
- **Tone** — exposure, contrast, sigmoid / filmic / curves,
  highlights / shadows
- **Geometric** — crop, rotate, straighten, perspective, lens
  correction (Lensfun)
- **Detail** — sharpening, noise reduction (incl. AI)
- **Output transform** — display transfer encode (sRGB / Rec.709 /
  PQ HDR / HLG HDR / linear)
- **Output** — JPEG / TIFF / PNG writers, EXIF writer
- **Metadata** — EXIF read, GPX geotagging, AI classification
- **Library** — importers, group operations

### Module shape (compile-time plug-in)

Each module is a self-contained C file declaring a static
`ap_module` struct:

- name, category
- parameter schema (used for sidecar serialization and UI rendering)
- default parameter values (the "identity" / disabled state)
- GPU dispatch (if image-pipeline-affecting): SPV, push schema,
  descriptor layout
- CPU/library hook (otherwise): callback functions
- UI draw function (renders module's controls in the right mode)

A central registry (`src/modules/registry.c`) lists all built-in
modules. Adding a module is:

1. Create `src/modules/<module>.c`
2. Add its shader to `shaders/meson.build` (if it has one)
3. Add include + registry entry in `registry.c`

No core engine changes. No new descriptor pool sizing, no new
pipeline graph code. The framework absorbs the new module.

### Loading model

**Compile-time only at v1.** Modules are statically linked. Runtime
plug-in (`.so` files in a directory) is reserved for v3+ when the
internal API is stable enough to commit to as an ABI.

## GPU pipeline architecture

### Working space

Default: **linear, sRGB / Rec.709 primaries**. Stored as a
configurable runtime parameter — switching to Rec.2020 or ACEScg
linear is a configuration change (matrices in the demosaic /
color-profile modules), not a code restructure.

### Working buffers

Two ping-pong `R16G16B16A16_SFLOAT` images sized to the source
photo. Float16 has the headroom for highlight recovery, tone
mapping, and wide-gamut color science that 8-bit linear cannot
express.

### Display image

`R8G8B8A8_UNORM` with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and two
views — UNORM for compute storage write, SRGB for sampling — so
ImGui's blender stays correct.

(For HDR display targets the format becomes `R10G10B10A2_UNORM_PACK32`
or `R16G16B16A16_SFLOAT`; the encode module's transfer-function
switch picks the right case.)

### Pipeline graph runner

`ap_pipeline_graph` owns the buffers and the ordered list of
modules. Each frame:

1. Resolve the photo's edit stack (expand pipeline references).
2. For each enabled module in order: bind, push constants, dispatch,
   barrier between stages.
3. Encode module produces the display image.

Modules can be enabled / disabled / reordered without touching the
runner. The runner enforces the ping-pong shape — modules don't
know which buffer is "input" or "output."

### Output transform

Final stage. Transfer-function push constant selects the output
encoding: sRGB at v1; PQ / HLG / linear-passthrough reserved for
HDR-display targets when those land.

## Edit model details

### `.aperture` sidecar (TOML)

```toml
[aperture]
schema = 1
version = "0.1.0"

# Photo's edit stack — ordered, reorderable. Mix of pipeline
# references and direct edits. Render order = top to bottom.
[[stack]]
type = "pipeline"
id   = "portrait-look-v3"

[[stack]]
type   = "edit"
module = "exposure"
[stack.params]
ev = 0.5

[[stack]]
type   = "edit"
module = "tone"
[stack.params]
contrast = 1.4
pivot    = 0.20

[metadata]
rating   = 4
flag     = "pick"        # or "reject" or null
color    = "yellow"
keywords = ["portrait", "studio", "lighting|softbox"]
title    = "..."
caption  = "..."
copyright = "..."

[capture]
# Read-once cache from source EXIF; not user-editable
camera = "..."
lens   = "..."
iso    = 400
shutter = "1/200"
aperture_value = 2.8
captured_at = 2026-04-01T14:22:13Z
gps = { lat = ..., lon = ..., alt = ... }
```

### Library db (SQLite)

Sketch of tables:

- `photos` — id, path, hash, capture_time, [cached metadata for fast
  filter / sort]
- `groups` — id, name, color, parent_id (groups can nest)
- `group_members` — group_id, photo_id
- `pipelines` — id, name, definition (TOML or normalized rows)
- `export_presets` — id, name, format, settings_blob
- `library_settings` — key, value

The db is a derived index. Losing it doesn't lose photo edits or
metadata (those live in sidecars). It does lose group membership and
pipeline definitions — those should be backed up separately if the
photographer wants robustness against db loss. (A v2+ "export
library settings to TOML" feature is a reasonable backup story.)

### Pipeline definitions

Pipelines are stored in the library db. Each definition is a TOML
blob (or normalized rows) listing the module instances it expands
into. A pipeline reference in a photo's stack is just an ID — the
db lookup happens at render time.

**Live propagation:** when the photographer edits a pipeline, the
db row updates and every photo using it re-renders next frame.

## Tech stack

### Language and runtime

- **C** (C17) — the project's primary language
- One C++ shim (`src/ui/imgui_bridge.cpp`) for cimgui's C++-only
  backend wrappers; isolated to that file
- **Vulkan 1.3** (synchronization2, dynamicRendering)
- **Build:** Meson, with `dep/` as the subprojects directory

### Dependencies kept

| Library | Job | Why kept |
|---|---|---|
| **LibRaw** | Raw decode + per-camera metadata | Decades of vendor reverse engineering; not reproducible |
| **Lensfun** | Lens corrections | The database is the value |
| **LittleCMS** | ICC profile math | ICC management is non-trivial against a real format |
| **libjpeg-turbo** | JPEG encode/decode | Real giant for JPEG |
| **libtiff** | TIFF encode/decode | Real giant; handles 8/16/32-bit + ICC + EXIF as IFD |
| **libpng** | PNG encode/decode | Real giant for PNG |
| **SQLite** | Library index | Embedded, single-file, the standard for this job |
| **cimgui** (ImGui docking branch) | UI shell | Deliberate aesthetic + functional fit |
| Vulkan SDK / GLFW | Graphics + window | The platform |

### Dependencies deliberately not used

| Library | Why dropped |
|---|---|
| OpenColorIO | Wraps color-space transforms aimed at VFX; our compute shaders apply them directly |
| OpenImageIO | Wraps libjpeg / libtiff / libpng; we use those directly |
| Exiv2 | Brings C++ runtime + XMP semantics we don't need (no external interop) |
| Expat / libxml2 | GPX parser is in-house — schema is small enough |

## Metadata & tagging

### Read paths

- **Source EXIF** — read on import via LibRaw, copied into the
  sidecar's `[capture]` section and into the library db's photo
  index. Source file is never modified.
- **No external XMP / .pp3 / .cof / etc.** Aperture does not read
  other tools' sidecars.

### Write paths

- **`.aperture` sidecar** — canonical record of all
  aperture-authored metadata and edit state.
- **Embedded EXIF on export** — JPEG / TIFF / PNG outputs carry
  EXIF (camera, exposure, lens, capture time, GPS coordinates
  authored via geotagging or manual entry). Implementation:
  in-house EXIF / TIFF-IFD writer (~few hundred lines, isolated
  in `src/modules/exif_writer.c`). No external EXIF library.
- **Embedded ICC on export** — via LittleCMS through the
  libjpeg-turbo / libtiff / libpng metadata APIs.

### Authored metadata fields

All stored in the `.aperture` sidecar:

- **Rating** (0–5)
- **Color label** (red / yellow / green / blue / purple)
- **Pick / reject flag**
- **Keywords** — flat and **hierarchical**. Default authoring
  separator `|`; configurable. On read, both `|` and `/` accepted.
- **Title, caption, headline**
- **Creator, copyright, rights, contact** — settable as
  per-import or per-camera defaults
- **Location** — city, state, country (textual) plus GPS coordinates
- **Capture time + originating timezone** — UTC plus IANA zone
  alongside the raw EXIF timestamp

### GPX geotagging

Workflow: photographer records a GPS track during a shoot;
aperture matches photo capture timestamps against the track and
writes interpolated GPS coordinates into the sidecar (and into
exported file EXIF).

**GPX format.** Small XML schema; the relevant subset:

```
gpx > trk > trkseg > trkpt[@lat, @lon] > ele, time
```

Multiple tracks per file, multiple segments per track, gaps
between segments treated as "no signal."

**Parser.** In-house, ~150 lines of C. Pulling in a general XML
library is unjustified for a schema this small.

**Timestamp interpolation.**

- Linear interpolation between adjacent `trkpt` timestamps for
  lat / lon / ele
- Camera capture time comes from EXIF `DateTimeOriginal` /
  `SubSecTimeOriginal`
- A photo whose timestamp falls inside a segment gets interpolated
  coordinates
- A photo within (configurable threshold, default 60s) of a segment
  endpoint can be snapped to the endpoint or refused
- A photo far from any segment is left untagged with a warning

**Clock offset reconciliation.** Camera clocks drift. The user
provides a fixed offset (e.g. "camera is +47s vs GPS UTC") that
aperture applies to all EXIF timestamps before interpolation.
Helper UI: shoot a photo of the GPS display; aperture computes the
offset from the visible time vs the photo's EXIF time.

**Timezone handling.** EXIF `DateTimeOriginal` is naive. GPX times
are UTC. The user provides the camera's timezone (default: system
timezone at import); aperture stores both the original timestamp
and the resolved UTC.

**Existing GPS data.** If a photo already has GPS coords in source
EXIF, aperture preserves them by default. Overwrite is opt-in per
session.

## File format support

### Input

- **Raw** — LibRaw-supported Bayer-pattern cameras at v1; X-Trans
  (Fuji) and Foveon at v3+
- **DNG** (via LibRaw) — first-class
- **JPEG / TIFF / PNG** — for editing already-processed images;
  same pipeline runs, demosaic stage is bypassed

### Output

- **JPEG** — 8-bit, embedded ICC, embedded EXIF, configurable quality
- **TIFF** — 8 / 16 / 32-bit, optional compression, embedded ICC + EXIF
- **PNG** — 8 / 16-bit, embedded ICC + EXIF

### Sidecar

- **`.aperture`** — TOML, one per source file. Edit stack +
  per-photo metadata. Schema-versioned.

### Library db

- **`<library>/library.aperture-db`** — SQLite, single file at the
  library root. Schema-versioned with explicit migrations after v1
  ships.

## Performance targets

Reference hardware: mid-range modern desktop GPU (RTX 3060 /
RX 6700-class) driving a 4K display.

- Slider drag latency: visible-region update under **16ms**
- Full-image re-render on parameter change: under **200ms** for a
  45MP raw
- Cold open of a raw file: under **2s** including decode + first
  pipeline render
- Library scan (cold, 10,000 photos): under **5s** with progressive
  thumbnail availability
- Idle CPU/GPU when not editing: effectively zero

These are targets, not contracts. Missing them by 50% on
pathological inputs is acceptable; missing them on common cases is
a bug.

## Platform support

- **Linux (primary).** Wayland-first. The reference development
  and CI platform.
- **Windows / macOS** — not v1. Vulkan + ImGui + chosen libraries
  are all portable; porting is mechanical when contributors are
  motivated.

## Scope phases

### v0 — Engine skeleton ✅

Status: implemented as PR-merged work on `dev`.

- Vulkan 1.3 device + ImGui shell + clear-color render
- Compute pipeline scaffolding (4 stages: process / tone / encode
  + LibRaw-processed input upload)
- Float16 working buffer + mutable-format display image
- Sliders for Exposure + Tone (Contrast / Pivot)
- Image displayed inside an ImGui window

### v1 — Framework + library + modes

The big one. After v1, aperture has a coherent application shape.

- **Module registry** (`ap_module` + central `registry.c`)
- **Pipeline graph runner** (`ap_pipeline_graph` with reorderable
  module list + ping-pong buffer runtime)
- **`ap_image`** (owns texture, raw metadata, edit stack,
  per-image rendering state)
- **Library** (root dir, SQLite db, photo index, import flow)
- **Mode shell** (Library / Photo / Export with contextual panels)
- **Sidecar v1 frozen** (TOML, `.aperture`, schema 1)
- **Pipelines** — define, save, apply, edit (live propagation)
- **Existing v0 modules ported** to module shape: decode, demosaic,
  exposure, tone, output_encode
- **Color correctness** — demosaic + camera color matrix + WB done
  right
- **EXIF orientation** as a geometric module
- **Canvas-as-primary** image display with pan/zoom
- **JPEG export module** with EXIF passthrough

### v1.5 — Metadata + GPX + groups

- Ratings, color labels, pick/reject flags
- Keywords (flat + hierarchical)
- Captions, copyright authoring
- Per-import authorship defaults
- GPX geotagging workflow
- Groups (user-defined, library-db tracked) with filter UI in
  Library mode
- Multi-photo selection + batch operations

### v2 — Color and lens

- Camera color profile selection (DCP / ICC)
- White balance sliders (temp / tint, scene-aware)
- Lens correction module (Lensfun)
- Histogram / waveform / vectorscope panels
- Soft proofing
- Crop / rotate / straighten / perspective module

### v3+ — AI and detail

- AI denoise (ONNX or ggml runtime)
- AI subject classification (for keywords)
- Capture sharpening / output sharpening modules
- Highlight reconstruction
- Multi-camera support (broader Bayer pattern + X-Trans)
- Runtime plug-in loading (when ABI is stable)

## Deliberately deferred

- Pixel-level retouching (heal, clone, dodge, burn, etc.)
- Layers, masks beyond crop / region-of-interest
- Cloud sync, web, mobile, multi-user, collaboration
- Multi-library catalog
- Tethered shooting
- Print module
- Slideshows / presentation modes

## Key decisions and constraints

- **Aperture is a library / culling / grading / export tool.** Not
  a photo editor. The "send to Photoshop for retouching" handoff
  is a feature, not a regret.
- **No false giants.** Wrappers (OCIO, OIIO, Exiv2) are dropped.
  Format-specific real libraries (libjpeg-turbo, libtiff, libpng,
  LibRaw, Lensfun, LittleCMS, SQLite) are kept.
- **No external interop.** Aperture's sidecar and library formats
  are its own. Migration to/from other tools is a third-party
  concern.
- **Module registry is the architecture.** Adding any feature is a
  single-file change.
- **Pipeline propagation is live.** Editing a pipeline updates all
  photos using it immediately. Manual snapshotting is opt-in.
- **Edit stack is reorderable.** Pipelines and direct edits coexist
  as items in the photo's stack; user can drag to reorder.
- **Library db is a derived index.** Filesystem + sidecars are
  authoritative for per-photo state. The db is for groups,
  pipelines, and fast browsing.
- **Vulkan 1.3 from the start** — synchronization2,
  dynamicRendering, float16 working buffers.
- **Compile-time modules at v1.** Runtime plug-ins are deferred.
- **No workarounds.** If a stage produces wrong output, fix the
  stage. If the architecture forces special-casing elsewhere, the
  architecture is wrong.

## Open questions

- **Library db location** — at the library root as
  `library.aperture-db`, or hidden in `.aperture/library.db`?
  Affects filesystem cleanliness vs discoverability.
- **Group nesting** — flat or hierarchical (`parent_id`)?
  Photographers commonly want nested groups.
- **Pipeline storage** — embedded TOML in the SQLite `pipelines`
  table or `<library>/pipelines/<id>.toml` files? File-based is
  more git-friendly for shareable presets but adds a watch-for-
  changes story.
- **Thumbnail cache** — inside SQLite (BLOB), in a separate
  `<library>/.aperture/thumbs/` directory, or per-photo
  `<photo>.thumb.jpg`?
- **Multi-photo edit application UX** — how the user applies a
  pipeline to a 50-photo selection. Atomic batch op? Per-photo
  undo entries?
- **Module ABI direction** — when we approach v3+ runtime plug-ins,
  the C API surface needs stability. Worth designing the v1
  internal `ap_module` shape with this in mind so the door isn't
  closed.
