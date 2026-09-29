// PVRTC1 (2bpp / 4bpp) decoder, following Imagination's reference decompressor.
#include "gles/gl.h"
#include <algorithm>
#include <vector>

namespace gles {

namespace {

struct Pixel32 {
    u8 r, g, b, a;
};
struct Pixel128S {
    s32 r, g, b, a;
};
struct Word {
    u32 mod, color;
};
struct WordIndices {
    int P[2], Q[2], R[2], S[2];
};

Pixel32 color_a(u32 c) {
    if (c & 0x8000)  // opaque RGB 554
        return {(u8)((c & 0x7c00) >> 10), (u8)((c & 0x3e0) >> 5), (u8)((c & 0x1e) | ((c & 0x1e) >> 4)), 0xf};
    return {(u8)(((c & 0xf00) >> 7) | ((c & 0xf00) >> 11)), (u8)(((c & 0xf0) >> 3) | ((c & 0xf0) >> 7)),
            (u8)(((c & 0xe) << 1) | ((c & 0xe) >> 2)), (u8)((c & 0x7000) >> 11)};
}

Pixel32 color_b(u32 c) {
    if (c & 0x80000000)  // opaque RGB 555
        return {(u8)((c & 0x7c000000) >> 26), (u8)((c & 0x3e00000) >> 21), (u8)((c & 0x1f0000) >> 16), 0xf};
    return {(u8)(((c & 0xf000000) >> 23) | ((c & 0xf000000) >> 27)), (u8)(((c & 0xf00000) >> 19) | ((c & 0xf00000) >> 23)),
            (u8)(((c & 0xf0000) >> 15) | ((c & 0xf0000) >> 19)), (u8)((c & 0x70000000) >> 27)};
}

void interpolate(Pixel32 P, Pixel32 Q, Pixel32 R, Pixel32 S, Pixel128S* out, int bpp) {
    const int ww = bpp == 2 ? 8 : 4, wh = 4;
    Pixel128S hP{P.r, P.g, P.b, P.a}, hQ{Q.r, Q.g, Q.b, Q.a}, hR{R.r, R.g, R.b, R.a}, hS{S.r, S.g, S.b, S.a};
    Pixel128S QmP{hQ.r - hP.r, hQ.g - hP.g, hQ.b - hP.b, hQ.a - hP.a};
    Pixel128S SmR{hS.r - hR.r, hS.g - hR.g, hS.b - hR.b, hS.a - hR.a};
    hP = {hP.r * ww, hP.g * ww, hP.b * ww, hP.a * ww};
    hR = {hR.r * ww, hR.g * ww, hR.b * ww, hR.a * ww};
    auto step = [](Pixel128S& a, const Pixel128S& d) { a.r += d.r, a.g += d.g, a.b += d.b, a.a += d.a; };
    if (bpp == 2) {
        for (int x = 0; x < ww; x++) {
            Pixel128S res{4 * hP.r, 4 * hP.g, 4 * hP.b, 4 * hP.a};
            Pixel128S dY{hR.r - hP.r, hR.g - hP.g, hR.b - hP.b, hR.a - hP.a};
            for (int y = 0; y < wh; y++) {
                Pixel128S& o = out[y * ww + x];
                o.r = (res.r >> 7) + (res.r >> 2);
                o.g = (res.g >> 7) + (res.g >> 2);
                o.b = (res.b >> 7) + (res.b >> 2);
                o.a = (res.a >> 5) + (res.a >> 1);
                step(res, dY);
            }
            step(hP, QmP);
            step(hR, SmR);
        }
    } else {
        for (int y = 0; y < wh; y++) {
            Pixel128S res{4 * hP.r, 4 * hP.g, 4 * hP.b, 4 * hP.a};
            Pixel128S dY{hR.r - hP.r, hR.g - hP.g, hR.b - hP.b, hR.a - hP.a};
            for (int x = 0; x < ww; x++) {
                Pixel128S& o = out[y * ww + x];
                o.r = (res.r >> 6) + (res.r >> 1);
                o.g = (res.g >> 6) + (res.g >> 1);
                o.b = (res.b >> 6) + (res.b >> 1);
                o.a = (res.a >> 4) + res.a;
                step(res, dY);
            }
            step(hP, QmP);
            step(hR, SmR);
        }
    }
}

void unpack_modulations(const Word& w, int ox, int oy, s32 values[16][8], s32 modes[16][8], int bpp) {
    u32 mode = w.color & 1;
    u32 bits = w.mod;
    if (bpp == 2) {
        if (mode) {
            if (bits & 1) {
                mode = (bits & (1u << 20)) ? 3 : 2;
                if (bits & (1u << 21)) bits |= (1u << 20);
                else bits &= ~(1u << 20);
            }
            if (bits & 2) bits |= 1;
            else bits &= ~1u;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++) {
                    modes[x + ox][y + oy] = mode;
                    if (((x ^ y) & 1) == 0) {
                        values[x + ox][y + oy] = bits & 3;
                        bits >>= 2;
                    }
                }
        } else {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 8; x++) {
                    modes[x + ox][y + oy] = mode;
                    values[x + ox][y + oy] = (bits & 1) ? 3 : 0;
                    bits >>= 1;
                }
        }
    } else {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                s32 v = bits & 3;
                if (mode) {
                    if (v == 1) v = 4;
                    else if (v == 2) v = 14;  // +10: punch-through alpha
                    else if (v == 3) v = 8;
                } else {
                    v *= 3;
                    if (v > 3) v -= 1;
                }
                values[y + oy][x + ox] = v;
                bits >>= 2;
            }
    }
}

s32 modulation(s32 values[16][8], s32 modes[16][8], int x, int y, int bpp) {
    if (bpp == 4) return values[x][y];
    static const s32 rep[4] = {0, 3, 5, 8};
    if (modes[x][y] == 0 || ((x ^ y) & 1) == 0) return rep[values[x][y]];
    if (modes[x][y] == 1)
        return (rep[values[x][y - 1]] + rep[values[x][y + 1]] + rep[values[x - 1][y]] + rep[values[x + 1][y]] + 2) / 4;
    if (modes[x][y] == 2) return (rep[values[x - 1][y]] + rep[values[x + 1][y]] + 1) / 2;
    return (rep[values[x][y - 1]] + rep[values[x][y + 1]] + 1) / 2;
}

void decompress_block(const Word& P, const Word& Q, const Word& R, const Word& S, Pixel32* out, int bpp) {
    const int ww = bpp == 2 ? 8 : 4, wh = 4;
    s32 values[16][8] = {}, modes[16][8] = {};
    unpack_modulations(P, 0, 0, values, modes, bpp);
    unpack_modulations(Q, ww, 0, values, modes, bpp);
    unpack_modulations(R, 0, wh, values, modes, bpp);
    unpack_modulations(S, ww, wh, values, modes, bpp);
    Pixel128S up_a[32], up_b[32];
    interpolate(color_a(P.color), color_a(Q.color), color_a(R.color), color_a(S.color), up_a, bpp);
    interpolate(color_b(P.color), color_b(Q.color), color_b(R.color), color_b(S.color), up_b, bpp);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            s32 mod = modulation(values, modes, x + ww / 2, y + wh / 2, bpp);
            bool punch = false;
            if (mod > 10) punch = true, mod -= 10;
            const Pixel128S& a = up_a[y * ww + x];
            const Pixel128S& b = up_b[y * ww + x];
            Pixel32 px;
            px.r = (u8)((a.r * (8 - mod) + b.r * mod) / 8);
            px.g = (u8)((a.g * (8 - mod) + b.g * mod) / 8);
            px.b = (u8)((a.b * (8 - mod) + b.b * mod) / 8);
            px.a = punch ? 0 : (u8)((a.a * (8 - mod) + b.a * mod) / 8);
            if (bpp == 2) out[y * ww + x] = px;
            else out[y + x * wh] = px;
        }
}

u32 twiddle(u32 xsize, u32 ysize, u32 xpos, u32 ypos) {
    u32 min_dim = xsize, max_value = ypos, tw = 0, src = 1, dst = 1;
    int shift = 0;
    if (ysize < xsize) min_dim = ysize, max_value = xpos;
    while (src < min_dim) {
        if (ypos & src) tw |= dst;
        if (xpos & src) tw |= dst << 1;
        src <<= 1;
        dst <<= 2;
        shift++;
    }
    max_value >>= shift;
    return tw | (max_value << (2 * shift));
}

void map_block(Pixel32* out, u32 width, const Pixel32* word, const WordIndices& w, int bpp) {
    const u32 ww = bpp == 2 ? 8 : 4, wh = 4;
    for (u32 y = 0; y < wh / 2; y++)
        for (u32 x = 0; x < ww / 2; x++) {
            out[((w.P[1] * wh) + y + wh / 2) * width + w.P[0] * ww + x + ww / 2] = word[y * ww + x];
            out[((w.Q[1] * wh) + y + wh / 2) * width + w.Q[0] * ww + x] = word[y * ww + x + ww / 2];
            out[((w.R[1] * wh) + y) * width + w.R[0] * ww + x + ww / 2] = word[(y + wh / 2) * ww + x];
            out[((w.S[1] * wh) + y) * width + w.S[0] * ww + x] = word[(y + wh / 2) * ww + x + ww / 2];
        }
}

void decompress(const u32* words, Pixel32* out, u32 width, u32 height, int bpp) {
    const u32 ww = bpp == 2 ? 8 : 4, wh = 4;
    int nx = (int)(width / ww), ny = (int)(height / wh);
    auto wrap = [](int n, int i) { return (i + n) % n; };
    Pixel32 pixels[32];
    for (int wy = -1; wy < ny - 1; wy++)
        for (int wx = -1; wx < nx - 1; wx++) {
            WordIndices ix;
            ix.P[0] = wrap(nx, wx), ix.P[1] = wrap(ny, wy);
            ix.Q[0] = wrap(nx, wx + 1), ix.Q[1] = wrap(ny, wy);
            ix.R[0] = wrap(nx, wx), ix.R[1] = wrap(ny, wy + 1);
            ix.S[0] = wrap(nx, wx + 1), ix.S[1] = wrap(ny, wy + 1);
            u32 off[4] = {twiddle(nx, ny, ix.P[0], ix.P[1]) * 2, twiddle(nx, ny, ix.Q[0], ix.Q[1]) * 2,
                          twiddle(nx, ny, ix.R[0], ix.R[1]) * 2, twiddle(nx, ny, ix.S[0], ix.S[1]) * 2};
            Word P{words[off[0]], words[off[0] + 1]}, Q{words[off[1]], words[off[1] + 1]}, R{words[off[2]], words[off[2] + 1]},
                S{words[off[3]], words[off[3] + 1]};
            decompress_block(P, Q, R, S, pixels, bpp);
            map_block(out, width, pixels, ix, bpp);
        }
}

}  // namespace

void pvrtc_decode(const u8* src, int width, int height, bool two_bpp, u8* rgba_out) {
    int bpp = two_bpp ? 2 : 4;
    u32 tw = std::max<u32>(width, two_bpp ? 16 : 8), th = std::max<u32>(height, 8);
    std::vector<Pixel32> buf(tw * th);
    // Small mips are padded to the minimum size; the data for them is still a full minimum block set.
    decompress(reinterpret_cast<const u32*>(src), buf.data(), tw, th, bpp);
    for (int y = 0; y < height; y++) std::memcpy(rgba_out + (size_t)y * width * 4, &buf[(size_t)y * tw], (size_t)width * 4);
}

}  // namespace gles
