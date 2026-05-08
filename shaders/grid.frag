#version 450

layout(location = 0) in  vec2 v_screen_uv;
layout(location = 0) out vec4 f_color;

layout(push_constant) uniform PushConstants {
    vec2  window_size_px;
    vec2  origin_px;          // top-left of the grid in window space
    vec4  bg_color;
    vec4  cell_color;
    vec4  selected_color;
    int   photo_count;
    int   selected_idx;
    int   cells_per_row;
    int   cell_size_px;
    int   cell_gap_px;
    int   border_px;
    int   _pad0, _pad1;
} pc;

void main() {
    vec2 px = v_screen_uv * pc.window_size_px - pc.origin_px;
    int  pitch = pc.cell_size_px + pc.cell_gap_px;

    if (px.x < 0.0 || px.y < 0.0) { f_color = pc.bg_color; return; }

    int col = int(floor(px.x / float(pitch)));
    int row = int(floor(px.y / float(pitch)));

    if (col < 0 || col >= pc.cells_per_row) { f_color = pc.bg_color; return; }

    int idx = row * pc.cells_per_row + col;
    if (idx < 0 || idx >= pc.photo_count) { f_color = pc.bg_color; return; }

    vec2 cell_origin = vec2(float(col * pitch), float(row * pitch));
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

    f_color = pc.cell_color;
}
