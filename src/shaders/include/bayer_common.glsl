// Shared Bayer helpers. The including shader must declare, before the
// include, an r16 readonly image2D named `in_image` and a push-constant
// block `pc` with `uvec4 channel_map`, `vec4 black_level` and
// `vec4 white_minus_black`.

// Bayer channel index (0=R, 1=G, 2=B, 3=G2) at a sensor coordinate.
uint channel_at(ivec2 p)
{
    return pc.channel_map[(p.y & 1) * 2 + (p.x & 1)];
}

// Black-level-normalized Bayer sample at a sensor coordinate. Clamps
// the coordinate first so channel parity and texel always agree at
// the image borders.
float bayer(ivec2 p, ivec2 size)
{
    p = clamp(p, ivec2(0), size - 1);
    uint  ch  = channel_at(p);
    float raw = imageLoad(in_image, p).r * 65535.0;
    return (raw - pc.black_level[ch]) / max(pc.white_minus_black[ch], 1.0);
}
