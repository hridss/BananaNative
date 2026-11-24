// Local album service unit test (hle_album.c, GitHub issue #3211): verifies all registered nn::album::SaveScreenshot overloads,
// 720p and 1080p RGBA8888 PNG output and pixel fidelity (ignoring source alpha), ImageOrientation rotations 0..3, collision-free
// atomic publishing, DM_ALBUM_DIR / DM_ROOT / DM_SAVE fallback order, and invalid-input / unwritable-directory error handling.
// The PNGs are read back with a small reader of its own (zlib; every chunk's CRC, all five row filters), so the test runs on the Mac and on an Android phone
// (NDK clang, run from adb shell: android/TESTING.md). DM_ALBUM_KEEP=<folder> also copies the 1080p picture there, for a check with another decoder.
#include "../hle_album.c"
#include <dirent.h>
#include <sys/stat.h>

#if !PLAT_APPLE
// The host's plat_android.c reads the ELF section the same way; this test links without it
extern const char __start_swforce[] __attribute__((weak, visibility("hidden"))), __stop_swforce[] __attribute__((weak, visibility("hidden")));
const void *plat_table(const char *name, unsigned long *size) {
    *size = 0;
    if (strcmp(name, "swforce") || !__start_swforce) return NULL;
    *size = (unsigned long)(__stop_swforce - __start_swforce);
    return __start_swforce;
}
#endif

static int checks, fails;
#define CHECK(c, ...) do { checks++; int _ok = !!(c); fails += !_ok; printf("%s ", _ok ? "OK" : "FAIL"); printf(__VA_ARGS__); printf("\n"); } while (0)

static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)plat_table("swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (strcmp(entries[i].name, name)) continue;
        count++; fn = entries[i].fn;
    }
    CHECK(count == 1 && fn, "hook registered once: %s", name);
    return count == 1 ? fn : NULL;
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[PATH_MAX];
        if ((size_t)snprintf(sub, sizeof sub, "%s/%s", path, de->d_name) < sizeof sub) rm_tree(sub);
    }
    closedir(d);
    rmdir(path);
}

static int list_pngs(const char *dir, char out[][PATH_MAX], int cap, int *tmp_count) {
    *tmp_count = 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        size_t len = strlen(de->d_name);
        if (len >= 4 && !strcmp(de->d_name + len - 4, ".tmp")) { (*tmp_count)++; continue; }
        if (len >= 4 && !strcmp(de->d_name + len - 4, ".png") && n < cap)
            snprintf(out[n++], PATH_MAX, "%s/%s", dir, de->d_name);
    }
    closedir(d);
    return n;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Decode an 8-bit RGB, non-interlaced PNG back into RGBA8888 (alpha 255) and return its dimensions; caller frees *out_rgba. Checks the signature, each chunk's CRC
// and that IEND ends the file
static int read_png_rgba(const char *path, size_t *out_w, size_t *out_h, uint8_t **out_rgba) {
    *out_rgba = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    int got = b && fread(b, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!got || len < 8 || memcmp(b, sig, 8)) { free(b); return 0; }
    size_t w = 0, h = 0, pos = 8, zlen = 0, zcap = 0;
    uint8_t *z = NULL;
    int ok = 0, ended = 0;
    while (pos + 12 <= (size_t)len) {
        uint32_t clen = rd32(b + pos);
        if (pos + 12 + clen > (size_t)len) break;
        const uint8_t *type = b + pos + 4, *data = b + pos + 8;
        if ((uint32_t)crc32(crc32(0L, type, 4), data, clen) != rd32(data + clen)) break;
        if (!memcmp(type, "IHDR", 4)) {
            if (clen != 13 || data[8] != 8 || data[9] != 2 || data[10] || data[11] || data[12]) break;
            w = rd32(data); h = rd32(data + 4);
        } else if (!memcmp(type, "IDAT", 4)) {
            if (zlen + clen > zcap) { zcap = (zlen + clen) * 2; uint8_t *nz = realloc(z, zcap); if (!nz) break; z = nz; }
            memcpy(z + zlen, data, clen); zlen += clen;
        } else if (!memcmp(type, "IEND", 4)) { ended = pos + 12 == (size_t)len; break; }
        pos += 12 + clen;
    }
    if (ended && w && h && z) {
        size_t stride = w * 3, rawlen = h * (stride + 1);
        uint8_t *raw = malloc(rawlen), *rgb = malloc(h * stride), *rgba = malloc(w * h * 4);
        uLongf dl = (uLongf)rawlen;
        if (raw && rgb && rgba && uncompress(raw, &dl, z, (uLong)zlen) == Z_OK && dl == rawlen) {
            ok = 1;
            for (size_t y = 0; ok && y < h; y++) {
                const uint8_t *in = raw + y * (stride + 1) + 1;
                uint8_t ft = in[-1], *cur = rgb + y * stride, *prev = y ? cur - stride : NULL;
                for (size_t i = 0; i < stride; i++) {
                    int a = i >= 3 ? cur[i - 3] : 0, up = prev ? prev[i] : 0, c = prev && i >= 3 ? prev[i - 3] : 0, pr = 0;
                    if (ft == 1) pr = a;
                    else if (ft == 2) pr = up;
                    else if (ft == 3) pr = (a + up) / 2;
                    else if (ft == 4) { int p = a + up - c, pa = abs(p - a), pb = abs(p - up), pc = abs(p - c); pr = pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
                    else if (ft != 0) { ok = 0; break; }
                    cur[i] = (uint8_t)(in[i] + pr);
                }
            }
            for (size_t i = 0; ok && i < w * h; i++) { rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1]; rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255; }
        }
        if (ok) { *out_w = w; *out_h = h; *out_rgba = rgba; rgba = NULL; }
        free(raw); free(rgb); free(rgba);
    }
    free(z); free(b);
    return ok;
}

static void set_px(uint8_t *buf, size_t w, size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = buf + (y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static int px_eq(const uint8_t *buf, size_t w, size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t *p = buf + (y * w + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

// Fill corners with distinct RGB values and alpha=0 (to verify alpha is stripped/opaque on save), over a gradient with noise so every row filter is exercised.
static uint8_t *make_test_rgba(size_t w, size_t h) {
    uint8_t *buf = calloc(w * h, 4);
    if (!buf) return NULL;
    uint32_t seed = 12345;
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++) {
            seed = seed * 1103515245u + 12345u;
            set_px(buf, w, x, y, (uint8_t)(x * 255 / w), (uint8_t)(y * 255 / h), (uint8_t)((x + y) & 0xff), (uint8_t)(seed >> 24));
            if ((seed >> 16) % 16 == 0) buf[(y * w + x) * 4 + 2] = (uint8_t)(seed >> 8);
        }
    set_px(buf, w, 0, 0, 255, 0, 0, 0);           // top-left: red
    set_px(buf, w, w - 1, 0, 0, 255, 0, 64);      // top-right: green
    set_px(buf, w, 0, h - 1, 0, 0, 255, 128);     // bottom-left: blue
    set_px(buf, w, w - 1, h - 1, 255, 255, 0, 0); // bottom-right: yellow
    return buf;
}

// Every pixel's RGB equals the source's (alpha opaque)
static int all_rgb_eq(const uint8_t *dec, const uint8_t *src, size_t npix) {
    for (size_t i = 0; i < npix; i++)
        if (memcmp(dec + i * 4, src + i * 4, 3) || dec[i * 4 + 3] != 255) return 0;
    return 1;
}

static void copy_file(const char *from, const char *dir) {
    char to[PATH_MAX];
    const char *base = strrchr(from, '/');
    if ((size_t)snprintf(to, sizeof to, "%s/%s", dir, base ? base + 1 : from) >= sizeof to) return;
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    char buf[65536]; size_t n;
    while (out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    if (out) { fclose(out); printf("kept %s\n", to); }
    if (in) fclose(in);
}

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

int main(void) {
    void (*fn_init)(void) = hook("_ZN2nn5album10InitializeEv");
    void (*fn_fini)(void) = hook("_ZN2nn5album8FinalizeEv");
    Result (*fn_save4)(const void *, size_t, int, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE");
    Result (*fn_save5)(const void *, size_t, int, int, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionE");
    Result (*fn_save7)(const void *, size_t, int, int, int, const void *, int) =
        hook("_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionEPKNS_7account3UidEi");
    Result (*fn_share)(const void *, size_t, int, int, int, const void *, const char *) =
        hook("_ZN2nn5album22SaveAndShareScreenshotEPKvmNS0_9ImageSizeENS0_16ImageOrientationENS0_17AlbumReportOptionERKNS_7account3UidEPKc");
    Result (*fn_share_id)(void *, const void *, size_t, int, int, const void *, const char *) =
        hook("_ZN2nn5album22SaveAndShareScreenshotEPNS0_7ShareIdEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionERKNS_7account3UidEPKc");
    Result (*fn_share_url)(char *, size_t, const void *) =
        hook("_ZN2nn5album21GetSharePostedLastUrlEPcmRKNS0_7ShareIdE");
    Result (*fn_show)(void) = hook("_ZN2nn5album14ShowAlbumFilesEv");
    Result (*fn_cur)(int) = hook("_ZN2nn5album21SaveCurrentScreenshotENS0_17AlbumReportOptionE");

    if (fn_init) fn_init();

    const char *base_tmp = getenv("TMPDIR");
    if (!base_tmp || !*base_tmp) base_tmp = "/tmp";
    char root[PATH_MAX];
    snprintf(root, sizeof root, "%s/t_album_XXXXXX", base_tmp);
    CHECK(mkdtemp(root) != NULL, "created temp test root");

    uint8_t *img1080 = make_test_rgba(1920, 1080);
    uint8_t *img720 = make_test_rgba(1280, 720);
    CHECK(img1080 && img720, "allocated 1080p and 720p RGBA test buffers");

    // 1. Pokopia path: 4-arg SaveScreenshot with ImageSize = 1 (1920x1080, 0x7e9000 bytes), nested DM_ALBUM_DIR created lazily.
    char album1080[PATH_MAX];
    snprintf(album1080, sizeof album1080, "%s/nested/album1080", root);
    setenv("DM_ALBUM_DIR", album1080, 1);
    unsetenv("DM_ROOT");
    unsetenv("DM_SAVE");
    double t0 = now_ms();
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u, 1, 3) == RES_OK, "1080p 4-arg SaveScreenshot succeeds (%.0f ms)", now_ms() - t0);
    char files[8][PATH_MAX]; int tmps = -1;
    int nfiles = list_pngs(album1080, files, 8, &tmps);
    CHECK(nfiles == 1 && tmps == 0, "1080p save wrote 1 PNG and left 0 temp files (got %d, %d)", nfiles, tmps);
    if (nfiles == 1) {
        size_t w = 0, h = 0; uint8_t *dec = NULL;
        CHECK(read_png_rgba(files[0], &w, &h, &dec) && w == 1920 && h == 1080, "1080p PNG decodes to 1920x1080 (got %zux%zu)", w, h);
        if (dec) {
            CHECK(px_eq(dec, w, 0, 0, 255, 0, 0, 255) &&
                  px_eq(dec, w, w - 1, 0, 0, 255, 0, 255) &&
                  px_eq(dec, w, 0, h - 1, 0, 0, 255, 255) &&
                  px_eq(dec, w, w - 1, h - 1, 255, 255, 0, 255),
                  "1080p PNG corners match RGBA input with opaque alpha");
            CHECK(all_rgb_eq(dec, img1080, w * h), "1080p PNG: every pixel's RGB matches the input");
            free(dec);
        }
        struct stat st;
        if (!stat(files[0], &st)) printf("1080p PNG: %lld bytes\n", (long long)st.st_size);
        const char *keep = getenv("DM_ALBUM_KEEP");
        if (keep && *keep) copy_file(files[0], keep);
    }

    // 2. Rapid saves in the same second do not collide or overwrite.
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u, 1, 3) == RES_OK, "second rapid SaveScreenshot succeeds");
    CHECK(fn_save7 && fn_save7(img720, 1280u * 720u * 4u, 0, 0, 2, NULL, 0) == RES_OK, "7-arg SaveScreenshot succeeds");
    nfiles = list_pngs(album1080, files, 8, &tmps);
    CHECK(nfiles == 3 && tmps == 0, "rapid saves keep all 3 distinct PNGs without temp leftovers (got %d, %d)", nfiles, tmps);

    // 3. All 4 ImageOrientation values on 1280x720 via 5-arg SaveScreenshot.
    for (int orient = 0; orient < 4; orient++) {
        char odir[PATH_MAX];
        snprintf(odir, sizeof odir, "%s/orient_%d", root, orient);
        setenv("DM_ALBUM_DIR", odir, 1);
        CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, orient, 2) == RES_OK, "720p orient=%d succeeds", orient);
        nfiles = list_pngs(odir, files, 8, &tmps);
        CHECK(nfiles == 1 && tmps == 0, "orient=%d wrote 1 PNG", orient);
        if (nfiles == 1) {
            size_t w = 0, h = 0; uint8_t *dec = NULL;
            size_t want_w = (orient == 1 || orient == 3) ? 720 : 1280;
            size_t want_h = (orient == 1 || orient == 3) ? 1280 : 720;
            CHECK(read_png_rgba(files[0], &w, &h, &dec) && w == want_w && h == want_h,
                  "orient=%d dimensions %zux%zu == %zux%zu", orient, w, h, want_w, want_h);
            if (dec) {
                int corners_ok = 0;
                if (orient == 0)
                    corners_ok = px_eq(dec, w, 0, 0, 255, 0, 0, 255) && px_eq(dec, w, w - 1, 0, 0, 255, 0, 255) &&
                                 px_eq(dec, w, 0, h - 1, 0, 0, 255, 255) && px_eq(dec, w, w - 1, h - 1, 255, 255, 0, 255);
                else if (orient == 1)   // 90 CW: TL->TR, TR->BR, BL->TL, BR->BL
                    corners_ok = px_eq(dec, w, w - 1, 0, 255, 0, 0, 255) && px_eq(dec, w, w - 1, h - 1, 0, 255, 0, 255) &&
                                 px_eq(dec, w, 0, 0, 0, 0, 255, 255) && px_eq(dec, w, 0, h - 1, 255, 255, 0, 255);
                else if (orient == 2)   // 180: TL->BR, TR->BL, BL->TR, BR->TL
                    corners_ok = px_eq(dec, w, w - 1, h - 1, 255, 0, 0, 255) && px_eq(dec, w, 0, h - 1, 0, 255, 0, 255) &&
                                 px_eq(dec, w, w - 1, 0, 0, 0, 255, 255) && px_eq(dec, w, 0, 0, 255, 255, 0, 255);
                else                    // 270 CW (90 CCW): TL->BL, TR->TL, BL->BR, BR->TR
                    corners_ok = px_eq(dec, w, 0, h - 1, 255, 0, 0, 255) && px_eq(dec, w, 0, 0, 0, 255, 0, 255) &&
                                 px_eq(dec, w, w - 1, h - 1, 0, 0, 255, 255) && px_eq(dec, w, w - 1, 0, 255, 255, 0, 255);
                CHECK(corners_ok, "orient=%d rotated corner pixels match", orient);
                free(dec);
            }
        }
    }

    // 4. Directory fallbacks: DM_ROOT/album and dirname(DM_SAVE)/album, and error when none set.
    char dm_root[PATH_MAX], dm_root_album[PATH_MAX], dm_save[PATH_MAX], dm_save_album[PATH_MAX];
    snprintf(dm_root, sizeof dm_root, "%s/root_fb/", root);
    snprintf(dm_root_album, sizeof dm_root_album, "%s/root_fb/album", root);
    unsetenv("DM_ALBUM_DIR");
    setenv("DM_ROOT", dm_root, 1);
    unsetenv("DM_SAVE");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == RES_OK, "DM_ROOT fallback succeeds");
    CHECK(list_pngs(dm_root_album, files, 8, &tmps) == 1, "DM_ROOT fallback wrote into <DM_ROOT>/album");

    snprintf(dm_save, sizeof dm_save, "%s/save_fb/save/", root);
    snprintf(dm_save_album, sizeof dm_save_album, "%s/save_fb/album", root);
    unsetenv("DM_ROOT");
    setenv("DM_SAVE", dm_save, 1);
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == RES_OK, "DM_SAVE sibling fallback succeeds");
    CHECK(list_pngs(dm_save_album, files, 8, &tmps) == 1, "DM_SAVE fallback wrote into <dirname(DM_SAVE)>/album");

    unsetenv("DM_SAVE");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_INVALID, "no album/root/save env fails cleanly");

    // 5. Validation and unwritable destination errors.
    char bad_dir[PATH_MAX];
    snprintf(bad_dir, sizeof bad_dir, "%s/empty_check", root);
    setenv("DM_ALBUM_DIR", bad_dir, 1);
    CHECK(fn_save4 && fn_save4(NULL, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_INVALID, "NULL pointer rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u - 1, 0, 0) == ALBUM_RES_INVALID, "truncated 720p buffer rejected");
    CHECK(fn_save4 && fn_save4(img1080, 1920u * 1080u * 4u - 4, 1, 0) == ALBUM_RES_INVALID, "truncated 1080p buffer rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 2, 0) == ALBUM_RES_INVALID, "unknown ImageSize=2 rejected");
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, -1, 0) == ALBUM_RES_INVALID, "negative ImageSize rejected");
    CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, 4, 0) == ALBUM_RES_INVALID, "invalid ImageOrientation=4 rejected");
    CHECK(fn_save5 && fn_save5(img720, 1280u * 720u * 4u, 0, -1, 0) == ALBUM_RES_INVALID, "negative ImageOrientation rejected");
    struct stat st;
    CHECK(stat(bad_dir, &st) != 0, "invalid calls do not create album directory");

    char not_a_dir[PATH_MAX];
    snprintf(not_a_dir, sizeof not_a_dir, "%s/regular_file", root);
    FILE *fp = fopen(not_a_dir, "w");
    CHECK(fp != NULL, "created regular file blocking album dir");
    if (fp) fclose(fp);
    setenv("DM_ALBUM_DIR", not_a_dir, 1);
    CHECK(fn_save4 && fn_save4(img720, 1280u * 720u * 4u, 0, 0) == ALBUM_RES_UNKNOWN, "file blocking DM_ALBUM_DIR returns album error");

    // 6. Offline/system-only album calls remain failing.
    uint8_t uid[16] = {0}, sid[32] = {0}; char url[32] = "keep";
    CHECK(fn_share && fn_share(img720, 1280u * 720u * 4u, 0, 0, 0, uid, "hi") == RES_FAIL, "SaveAndShareScreenshot fails");
    CHECK(fn_share_id && fn_share_id(sid, img720, 1280u * 720u * 4u, 0, 0, uid, "hi") == RES_FAIL, "SaveAndShareScreenshot (ShareId) fails");
    CHECK(fn_share_url && fn_share_url(url, sizeof url, sid) == RES_FAIL && url[0] == 0, "GetSharePostedLastUrl clears buffer and fails");
    CHECK(fn_show && fn_show() == RES_FAIL, "ShowAlbumFiles fails");
    CHECK(fn_cur && fn_cur(0) == RES_FAIL, "SaveCurrentScreenshot fails");

    if (fn_fini) fn_fini();
    free(img1080);
    free(img720);
    rm_tree(root);
    printf("t_album: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
