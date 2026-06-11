// Bilinear sample from `in_image` (rgba16f readonly image2D, declared
// by the including shader) at a floating-point pixel coordinate, with
// the four taps clamped to the image bounds.
vec4 sample_bilinear(vec2 coord, ivec2 size)
{
    vec2  f  = fract(coord);
    ivec2 c0 = ivec2(floor(coord));

    ivec2 c00 = clamp(c0,              ivec2(0), size - 1);
    ivec2 c10 = clamp(c0 + ivec2(1,0), ivec2(0), size - 1);
    ivec2 c01 = clamp(c0 + ivec2(0,1), ivec2(0), size - 1);
    ivec2 c11 = clamp(c0 + ivec2(1,1), ivec2(0), size - 1);

    vec4 s00 = imageLoad(in_image, c00);
    vec4 s10 = imageLoad(in_image, c10);
    vec4 s01 = imageLoad(in_image, c01);
    vec4 s11 = imageLoad(in_image, c11);

    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}
