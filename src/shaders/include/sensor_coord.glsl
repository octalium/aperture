// Maps an output (display-space) pixel back to its sensor coordinate
// for an EXIF flip code (dcraw / LibRaw convention):
//   0 = no rotation
//   3 = 180°
//   5 = 90° CCW (sensor was rotated 90° CW for the shot)
//   6 = 90° CW  (sensor was rotated 90° CCW for the shot)
ivec2 sensor_coord_of(ivec2 out_coord, ivec2 sensor_size, uint flip)
{
    if (flip == 3u) {
        return sensor_size - ivec2(1) - out_coord;
    } else if (flip == 5u) {
        return ivec2(sensor_size.x - 1 - out_coord.y, out_coord.x);
    } else if (flip == 6u) {
        return ivec2(out_coord.y, sensor_size.y - 1 - out_coord.x);
    }
    return out_coord;
}
