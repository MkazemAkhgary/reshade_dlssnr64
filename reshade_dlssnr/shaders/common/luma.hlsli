#ifndef DLSSNR_LUMA_HLSLI
#define DLSSNR_LUMA_HLSLI

static const float3 kL709 = float3(0.2126, 0.7152, 0.0722);
static const float3 kLuma709 = kL709;
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);

static const float kCbScale = 2.0 * (1.0 - kL709.z);
static const float kCrScale = 2.0 * (1.0 - kL709.x);
static const float kGCb = (kL709.z * kCbScale) / kL709.y;
static const float kGCr = (kL709.x * kCrScale) / kL709.y;

#ifdef DLSSNR_BINDINGS_HLSI
float3 HdrLumaWeights()
{
    int fmt = (int)(kFormat + 0.5);
    return fmt == 2 ? kLuma2020 : kLuma709;
}
#endif

#endif
