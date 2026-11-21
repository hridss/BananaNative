// SPDX-License-Identifier: MIT
// the splat key's depth code (reversed Z, nearer = larger): the float's top bits, clamp((bits >> sh) - off, 1, mx); z <= 0 or not finite: 1. The host's layout
// (vka_fgmv.c, per render size): sh 16, off 12288, mx 4095 (the 12-bit code: exponent and top 7 mantissa bits) up to 2^20 texels, else as many code bits as the index leaves
uint zcode(float z, uint sh, int off, int mx) {
    if (!(z > 0.0) || isinf(z)) return 1u;
    return uint(clamp(int(floatBitsToUint(z) >> sh) - off, 1, mx));
}
