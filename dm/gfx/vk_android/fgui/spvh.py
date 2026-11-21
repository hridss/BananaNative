# SPDX-License-Identifier: MIT
import sys, struct
out = ['// SPDX-License-Identifier: MIT', '// SPIR-V of the frame generation UI / cut compute shaders (vka_fgui.c), made from fgui/*.comp (with fgui/common.glsl) by', '// glslc --target-env=vulkan1.1 -O (NDK 25.2 shader-tools). Regenerate after editing the GLSL.', '#pragma once', '#include <stdint.h>']
for n in sys.argv[1:]:
    b = open(n + '.spv', 'rb').read(); w = struct.unpack('<%dI' % (len(b) // 4), b)
    out.append('static const uint32_t vka_fgui_%s_spv[%d] = {' % (n, len(w)))
    for i in range(0, len(w), 12): out.append('    ' + ', '.join('0x%08x' % x for x in w[i:i + 12]) + ',')
    out.append('};')
open('../vka_fgui_spv.h', 'w', newline='\n').write('\n'.join(out) + '\n')
