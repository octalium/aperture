#version 450

layout(location = 0) in  vec2 v_screen_uv;
layout(location = 0) out vec4 f_color;

layout(set = 0, binding = 0) uniform sampler2D u_image;

layout(push_constant) uniform PushConstants {
    vec2  image_size_px;   // the cropped sub-region's pixel size
    vec2  window_size_px;
    vec2  pan_px;
    float zoom;
    float fit_scale;
    vec4  bg_color;
    vec4  crop_uv;         // .xy = crop origin, .zw = crop size (normalized)
} pc;

void main() {
    vec2  screen_px  = v_screen_uv * pc.window_size_px;
    vec2  win_center = pc.window_size_px * 0.5;
    float scale      = pc.fit_scale * pc.zoom;

    // img_px is in *cropped-image* pixel space; image_size_px is the
    // crop's pixel size, so fit / pan / zoom all operate on the
    // visible crop, not the full frame.
    vec2 img_px = (screen_px - win_center - pc.pan_px) / scale
                + pc.image_size_px * 0.5;

    if (img_px.x < 0.0 || img_px.x >= pc.image_size_px.x ||
        img_px.y < 0.0 || img_px.y >= pc.image_size_px.y) {
        f_color = pc.bg_color;
        return;
    }

    // Map the cropped-local UV back into the full texture: the crop
    // is a framing window — the pipeline rendered the whole frame,
    // we only show pc.crop_uv of it.
    vec2 local_uv = img_px / pc.image_size_px;
    vec2 tex_uv   = pc.crop_uv.xy + local_uv * pc.crop_uv.zw;
    f_color = texture(u_image, tex_uv);
}
