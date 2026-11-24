// H.264 (AVC) decoding for the movie decoder (hle_movie.c), on Apple's VideoToolbox (hle_movie_avc.c) or Android's MediaCodec (plat/movie_avc_android.c)
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct mavc mavc;
mavc *mavc_new(void);
void mavc_free(mavc *a);
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n);   // SPS / PPS (Configure's csd-0 / csd-1) with Annex B start codes
void mavc_avcc(mavc *a, const uint8_t *p, size_t n);         // SPS / PPS from an AVCDecoderConfigurationRecord (the MP4 avcC box)
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us);   // one access unit with Annex B start codes; 0 = decoded (or nothing to decode), -1 = error (no start code)
// the next picture in presentation order, as NV12 (Y stride w, then interleaved UV at w*h, stride w; a smaller picture is padded with black, a larger one cropped):
// 1 = a picture was written (its time in *pts_us), 0 = none yet; with flush every queued picture comes out (end of stream)
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us);
int mavc_queued(const mavc *a);
// The colour matrix and range of the pictures' NV12, for the game's YUV → RGB conversion (hle_movie.c md_format's nv12-colorspace): BT.709 as the stream declares it, undeclared (or no
// SPS yet) from h = 720 lines up; full range as the pictures hold it (VideoToolbox: always video range; MediaCodec: the stream's)
int mavc_bt709(const mavc *a, int h);
int mavc_full(const mavc *a);
void mavc_set_rgba(mavc *a);   // pictures come out as 4-byte RGBA (stride w * 4) instead of NV12: the native texture mode
void mavc_drop(mavc *a);   // a flush / seek: queued pictures are discarded
