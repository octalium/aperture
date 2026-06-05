#ifndef APERTURE_GPU_PIPELINE_GRAPH_H
#define APERTURE_GPU_PIPELINE_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "edit/stack.h"
#include "gpu/gpu.h"
#include "gpu/texture.h"
#include "io/raw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_pipeline_graph ap_pipeline_graph;

// Presentation ring depth: the display render target is copied into one of
// these slots per render so the swapchain compositor can sample a finished
// image while a new render proceeds. 2 frames-in-flight + 1 render = 3.
#define AP_DISPLAY_SLOTS 3

// Forward decl from modules/module.h to avoid pulling the header from a
// gpu-layer file. Callers include modules/module.h to get the full type.
typedef struct ap_module ap_module;

// Construct a graph from the photo's edit stack. The graph runs:
//   [demosaic-or-passthrough] + (user stack entries) + [encode]
// where the transport modules are inserted by the graph, not the user.
// Disabled user entries (except demosaic, see below) are included in
// the graph with their skip flag set; ap_pipeline_graph_set_stage_skip
// toggles them at runtime without a rebuild. Disabled demosaic entries
// are replaced by raw_passthrough (format incompatibility prevents
// copy-passthrough for that stage).
//
// Allocates the float16 ping-pong working buffers and the UNORM display
// image at `output_width × output_height`, which may differ from the
// input texture's dimensions (e.g. when the first stage applies an
// EXIF orientation rotation).
//
// `meta` is per-image static metadata (camera color matrix, black
// levels, sensor dims, flip code, etc.) - copied into the graph at
// create time and passed to each module's pack_push. May be NULL for
// chains that don't need it.
ap_pipeline_graph *ap_pipeline_graph_create(ap_gpu *g,
                                            ap_texture *input,
                                            int output_width,
                                            int output_height,
                                            const ap_edit_stack *stack,
                                            const ap_raw_metadata *meta);
void ap_pipeline_graph_destroy(ap_pipeline_graph *graph);

// Whether a render is needed: true if the chain has never been recorded
// or the edit stack changed since the last record (mirrors the record
// short-circuit). The async pump uses this to avoid recording an empty
// command buffer every idle frame.
bool ap_pipeline_graph_needs_render(const ap_pipeline_graph *graph,
                                    const ap_edit_stack *stack);

// Records the full chain for one frame. Each module's pack_push gets
// the parameter slots of the edit-stack entry that scheduled it
// (NULL for transport modules), then a dispatch + a barrier. Returns 1
// when it recorded a dispatch (the caller should present-copy the result),
// 0 when nothing changed and it recorded nothing (the previous render is
// still valid), or -1 on error.
int ap_pipeline_graph_record(ap_pipeline_graph *graph, VkCommandBuffer cmd,
                             const ap_edit_stack *stack);

// Toggle the runtime skip flag for the stage whose edit-stack entry
// index is `entry_idx`. A skipped stage copies its input through to
// its output unchanged, preserving the ping-pong buffer invariant,
// rather than dispatching its compute shader. Resets the frame
// snapshot so the next ap_pipeline_graph_record re-records.
// Returns 0 if the stage was found and updated, -1 if no stage with
// that entry_idx exists in the graph (e.g. the module was disabled at
// graph build time and not included — caller must rebuild instead).
int ap_pipeline_graph_set_stage_skip(ap_pipeline_graph *graph,
                                     int entry_idx, bool skip);

// Async off-screen render + readback, for export: keeps the main thread
// unblocked during the (expensive, full-res) render and keeps exactly one
// export GPU submission in flight at a time.
typedef struct ap_gpu_readback ap_gpu_readback;

// Record the compute chain + a copy of display_image into a staging
// buffer in one command buffer, submit it with a fence, and return
// immediately (no GPU wait). Poll with ap_gpu_readback_poll. The graph
// (its display_image) must stay alive until the poll returns done.
// Returns NULL on error.
ap_gpu_readback *ap_pipeline_graph_readback_begin(ap_pipeline_graph *graph,
                                                  const ap_edit_stack *stack);

// Poll an in-flight readback (non-blocking). Returns 0 while still
// rendering (retry next frame), 1 when complete — copies the sRGB RGBA8
// pixels into `out_pixels` (>= width*height*4 bytes) and frees the
// handle (do not poll again) — or -1 on error (handle freed). Main thread.
int ap_gpu_readback_poll(ap_gpu_readback *rb, void *out_pixels, size_t out_size);

// Abandon an in-flight readback without retrieving pixels (cancel path).
// Waits for the fence first (the GPU may still reference the staging
// buffer), then frees. Safe on NULL.
void ap_gpu_readback_destroy(ap_gpu_readback *rb);

// Dispatch the compute chain once into display_image on a transient,
// one-shot command buffer, decoupled from the swapchain frame loop.
// Needed by off-screen consumers (export): a photo opened solely for
// export is never the bound current_graph, so the per-frame submit never
// renders it — without this its display_image is undefined (black).
// Records via ap_pipeline_graph_record, so output matches the canvas.
// Synchronous (waits on the queue). Main thread only. Returns 0 on
// success, -1 on failure.
int ap_pipeline_graph_render_once(ap_pipeline_graph *graph,
                                  const ap_edit_stack *stack);

// Copy the current display image (final stage's output) into a CPU
// buffer. `out_pixels` must hold at least `output_width * output_height
// * 4` bytes; data is written as 8-bit RGBA, sRGB-encoded (the bytes
// the encode shader produced). Returns 0 on success. Synchronous -
// waits on the device before reading. The display image must already
// have been rendered (the frame loop for the live graph, or
// ap_pipeline_graph_render_once for an off-screen one).
int ap_pipeline_graph_readback(ap_pipeline_graph *graph,
                               void *out_pixels, size_t out_size);

// Downsample + readback for the close-path thumbnail. Blits the
// display image into a pre-allocated thumb image (linear filter -
// GPU does the downsample) and reads back the small image, dodging
// the PCIe cost of pulling the full-resolution output. `out_pixels`
// must hold at least `thumb_width * thumb_height * 4` bytes; the
// function returns the actual dims in `*out_w` / `*out_h`. Same
// sRGB-encoded RGBA8 format as the full readback. Synchronous.
int ap_pipeline_graph_readback_thumb(ap_pipeline_graph *graph,
                                     void *out_pixels, size_t out_size,
                                     int *out_w, int *out_h);

int ap_pipeline_graph_thumb_width(const ap_pipeline_graph *graph);
int ap_pipeline_graph_thumb_height(const ap_pipeline_graph *graph);

// Presentation ring. The render target (display image) is copied into one
// of AP_DISPLAY_SLOTS slots so the swapchain compositor can sample a
// finished image while a new render proceeds, instead of sampling the live
// render target. ap_pipeline_graph_present_copy records that copy (display
// image -> slot) plus the barriers that leave the slot ready for fragment
// sampling; the caller selects the slot and owns the CPU-side recycle gate
// (do not copy into a slot an in-flight frame still samples). slot_count /
// slot_view expose the slots for descriptor setup.
void ap_pipeline_graph_present_copy(ap_pipeline_graph *graph,
                                    VkCommandBuffer cmd, int slot);
int         ap_pipeline_graph_slot_count(const ap_pipeline_graph *graph);
VkImageView ap_pipeline_graph_slot_view(const ap_pipeline_graph *graph, int slot);

// Advance the round-robin present slot: returns the slot index the next
// present-copy should target and records it as the new front slot. The
// caller copies into that slot (ap_pipeline_graph_present_copy) and binds
// the compositor to it. front_slot reports the slot last presented.
int ap_pipeline_graph_next_slot(ap_pipeline_graph *graph);
int ap_pipeline_graph_front_slot(const ap_pipeline_graph *graph);

// The display image (final output) - for sampling via ImGui or canvas.
VkImageView   ap_pipeline_graph_output_view(const ap_pipeline_graph *graph);
VkSampler     ap_pipeline_graph_output_sampler(const ap_pipeline_graph *graph);
VkImageLayout ap_pipeline_graph_output_layout(const ap_pipeline_graph *graph);
int           ap_pipeline_graph_output_width(const ap_pipeline_graph *graph);
int           ap_pipeline_graph_output_height(const ap_pipeline_graph *graph);

// Read the GPU histogram accumulated by the most recent dispatch.
//
// `out_bins` must point to a caller-allocated array of 1024 uint32_t
// values in four contiguous runs of 256:
//   [0..255]   R channel
//   [256..511] G channel
//   [512..767] B channel
//   [768..1023] Luma (Rec.709)
//
// Returns true if histogram data is available (at least one dispatch has
// completed), false if the graph has never been dispatched or has no
// histogram pass. Does NOT synchronise with the GPU — the caller must
// ensure the previous frame's fence has been waited on before calling.
bool ap_pipeline_graph_histogram_read(const ap_pipeline_graph *graph,
                                      uint32_t out_bins[1024]);

#ifdef __cplusplus
}
#endif

#endif
