#version 450

layout(location = 0) in  vec2 v_screen_uv;
layout(location = 0) out vec4 f_color;

layout(set = 0, binding = 0) uniform sampler2D u_image;

layout(push_constant) uniform PushConstants {
    vec2  image_size_px;   // framed output size (after the viewport)
    vec2  window_size_px;
    vec2  pan_px;
    float zoom;
    float fit_scale;
    vec4  bg_color;
    vec4  crop_rect;       // x0, y0, x1, y1 (normalized)
    vec4  vp_params;       // rotation_rad, flip_x, flip_y, autozoom
    vec2  source_size_px;  // full rendered image size
} pc;

void main() {
    vec2  screen_px  = v_screen_uv * pc.window_size_px;
    vec2  win_center = pc.window_size_px * 0.5;
    float scale      = pc.fit_scale * pc.zoom;

    // framed_px: pixel in the framed-output image. fit / pan / zoom
    // operate on the framed (post-viewport) size.
    vec2 framed_px = (screen_px - win_center - pc.pan_px) / scale
                   + pc.image_size_px * 0.5;

    if (framed_px.x < 0.0 || framed_px.x >= pc.image_size_px.x ||
        framed_px.y < 0.0 || framed_px.y >= pc.image_size_px.y) {
        f_color = pc.bg_color;
        return;
    }

    // Viewport backward map: framed-output pixel -> source UV.
    // Mirrors ap_viewport_resample_rgba8 (src/edit/viewport.c) so the
    // on-screen framing and the exported framing agree.
    vec2 t = framed_px / pc.image_size_px;          // [0,1] in framed
    if (pc.vp_params.y > 0.5) t.x = 1.0 - t.x;      // flip_x
    if (pc.vp_params.z > 0.5) t.y = 1.0 - t.y;      // flip_y

    vec2 crop_origin = pc.crop_rect.xy;
    vec2 crop_size   = pc.crop_rect.zw - pc.crop_rect.xy;
    vec2 rf = crop_origin + t * crop_size;          // rotated-frame [0,1]

    // Orientation backward map: rotate in pixel space, un-zoom.
    float zoomf = max(pc.vp_params.w, 1e-4);
    vec2  c   = (rf - 0.5) * pc.source_size_px / zoomf;
    float ang = -pc.vp_params.x;
    float cs  = cos(ang);
    float sn  = sin(ang);
    vec2  src_c  = vec2(c.x * cs - c.y * sn, c.x * sn + c.y * cs);
    vec2  src_uv = (src_c + pc.source_size_px * 0.5) / pc.source_size_px;

    if (src_uv.x < 0.0 || src_uv.x > 1.0 ||
        src_uv.y < 0.0 || src_uv.y > 1.0) {
        f_color = pc.bg_color;
        return;
    }
    f_color = texture(u_image, src_uv);
}
