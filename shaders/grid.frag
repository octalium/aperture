#version 450
#extension GL_EXT_nonuniform_qualifier : require

#define MAX_THUMBS 4096

layout(location = 0) in  vec2 v_screen_uv;
layout(location = 0) out vec4 f_color;

layout(set = 0, binding = 0) uniform sampler2D u_thumbs[MAX_THUMBS];

layout(push_constant) uniform PushConstants {
    vec2  window_size_px;
    vec2  origin_px;          // top-left of the grid in window space
    vec4  bg_color;
    vec4  selected_color;
    int   photo_count;
    int   selected_idx;
    int   cells_per_row;
    int   cell_size_px;
    int   cell_gap_x_px;
    int   cell_gap_y_px;
    int   border_px;
    int   hover_idx;
} pc;

// Aspect-correct UV inside a square cell. Letterbox bands fall back
// to the bg_color so portrait/landscape thumbnails don't get
// stretched to the cell square.
vec4 sample_cell(int idx, vec2 in_cell) {
    vec2 cell = vec2(float(pc.cell_size_px));
    vec2 thumb_size = vec2(textureSize(u_thumbs[nonuniformEXT(idx)], 0));
    if (thumb_size.x <= 0.0 || thumb_size.y <= 0.0) return pc.bg_color;

    float scale = min(cell.x / thumb_size.x, cell.y / thumb_size.y);
    vec2  fit   = thumb_size * scale;
    vec2  pad   = (cell - fit) * 0.5;

    if (in_cell.x < pad.x || in_cell.y < pad.y ||
        in_cell.x >= cell.x - pad.x || in_cell.y >= cell.y - pad.y) {
        return pc.bg_color;
    }
    vec2 uv = (in_cell - pad) / fit;
    return texture(u_thumbs[nonuniformEXT(idx)], uv);
}

void main() {
    vec2 px = v_screen_uv * pc.window_size_px - pc.origin_px;
    int  pitch_x = pc.cell_size_px + pc.cell_gap_x_px;
    int  pitch_y = pc.cell_size_px + pc.cell_gap_y_px;

    if (px.x < 0.0 || px.y < 0.0) { f_color = pc.bg_color; return; }

    int col = int(floor(px.x / float(pitch_x)));
    int row = int(floor(px.y / float(pitch_y)));

    if (col < 0 || col >= pc.cells_per_row) { f_color = pc.bg_color; return; }

    int idx = row * pc.cells_per_row + col;
    if (idx < 0 || idx >= pc.photo_count) { f_color = pc.bg_color; return; }

    vec2 cell_origin = vec2(float(col * pitch_x), float(row * pitch_y));
    vec2 in_cell     = px - cell_origin;
    if (in_cell.x >= float(pc.cell_size_px) ||
        in_cell.y >= float(pc.cell_size_px)) {
        f_color = pc.bg_color;
        return;
    }

    if (idx == pc.selected_idx) {
        float b = float(pc.border_px);
        float sz = float(pc.cell_size_px);
        if (in_cell.x < b || in_cell.x > sz - b ||
            in_cell.y < b || in_cell.y > sz - b) {
            f_color = pc.selected_color;
            return;
        }
    }

    vec4 cell_color = sample_cell(idx, in_cell);

    // Hover: brighten the cell with a subtle additive tint so the user
    // can see which cell is under the cursor without obscuring the thumb.
    if (idx == pc.hover_idx && idx != pc.selected_idx) {
        cell_color.rgb += vec3(0.08);
    }

    f_color = cell_color;
}
