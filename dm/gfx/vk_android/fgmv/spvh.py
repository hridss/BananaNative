# SPDX-License-Identifier: MIT
import sys, struct
out = ['// SPDX-License-Identifier: MIT', '// SPIR-V of the motion-vector frame generator compute shaders (vka_fgmv.c), made from fgmv/*.comp (with fgmv/key.glsl) by', '// glslc --target-env=vulkan1.1 -O (NDK 25.2 shader-tools). Regenerate after editing the GLSL.', '#pragma once', '#include <stdint.h>']
for n in sys.argv[1:]:
    b = open(n + '.spv', 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b)
    out.append('static const uint32_t vka_fgmv_%s_spv[%d] = {' % (n, len(w)))
    for i in range(0, len(w), 12): out.append('    ' + ', '.join('0x%08x' % x for x in w[i:i + 12]) + ',')
    out.append('};')
open('../vka_fgmv_spv.h', 'w', newline='\n').write('\n'.join(out) + '\n')
