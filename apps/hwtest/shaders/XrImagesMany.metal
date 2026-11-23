#include <metal_stdlib>
using namespace metal;

#define XR_IMAGE(n) texture2d<float, access::write> t##n [[texture(n)]]

/// gpu.gm20b.xr_images_many: (0.5, 0.25, -0.25, 0.75) written at the pixel into each of the sixteen bound textures
fragment float4 xr_write_sixteen(float4 position [[position]], XR_IMAGE(0), XR_IMAGE(1), XR_IMAGE(2), XR_IMAGE(3),
                                 XR_IMAGE(4), XR_IMAGE(5), XR_IMAGE(6), XR_IMAGE(7), XR_IMAGE(8), XR_IMAGE(9),
                                 XR_IMAGE(10), XR_IMAGE(11), XR_IMAGE(12), XR_IMAGE(13), XR_IMAGE(14), XR_IMAGE(15))
{
    const uint2 p = uint2(position.xy);
    const float4 v = float4(0.5f, 0.25f, -0.25f, 0.75f);
    t0.write(v, p);
    t1.write(v, p);
    t2.write(v, p);
    t3.write(v, p);
    t4.write(v, p);
    t5.write(v, p);
    t6.write(v, p);
    t7.write(v, p);
    t8.write(v, p);
    t9.write(v, p);
    t10.write(v, p);
    t11.write(v, p);
    t12.write(v, p);
    t13.write(v, p);
    t14.write(v, p);
    t15.write(v, p);
    return float4(0.0f);
}
