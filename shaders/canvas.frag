#version 450

layout(location = 0) in  vec2 v_screen_uv;
layout(location = 0) out vec4 f_color;

layout(set = 0, binding = 0) uniform sampler2D u_image;

layout(push_constant) uniform PushConstants {
    vec2  image_size_px;
    vec2  window_size_px;
    vec2  pan_px;
    float zoom;
    float fit_scale;
    vec4  bg_color;
} pc;

void main() {
    vec2  screen_px  = v_screen_uv * pc.window_size_px;
    vec2  win_center = pc.window_size_px * 0.5;
    float scale      = pc.fit_scale * pc.zoom;

    vec2 img_px = (screen_px - win_center - pc.pan_px) / scale
                + pc.image_size_px * 0.5;

    if (img_px.x < 0.0 || img_px.x >= pc.image_size_px.x ||
        img_px.y < 0.0 || img_px.y >= pc.image_size_px.y) {
        f_color = pc.bg_color;
        return;
    }

    f_color = texture(u_image, img_px / pc.image_size_px);
}
