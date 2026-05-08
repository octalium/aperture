#version 450

layout(location = 0) out vec2 v_screen_uv;

// Fullscreen triangle: vertex 0 at (-1,-1), 1 at (3,-1), 2 at (-1,3).
// Intersected with the [-1,1] viewport, it covers the whole framebuffer.
void main() {
    vec2 pos = vec2(
        gl_VertexIndex == 1 ? 3.0 : -1.0,
        gl_VertexIndex == 2 ? 3.0 : -1.0
    );
    gl_Position = vec4(pos, 0.0, 1.0);

    // Vulkan NDC has y pointing down, so y=0 here is the top of the
    // swapchain — matches our image origin convention.
    v_screen_uv = pos * 0.5 + 0.5;
}
