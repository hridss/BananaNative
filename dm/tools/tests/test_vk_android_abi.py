"""The Android Vulkan shader library ABI (dm/gfx/nvn/dmsh_abi_vk.h): the C structs have the sizes the build packs, every binding kind the build may write has a number,
the library version header is current, and the SPIR-V rewrites keep a module valid. No game files needed; the spirv-val check is skipped without SPIRV-Tools."""
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import vk_android_spv as spv   # noqa: E402

HEADER = REPO / 'dm/gfx/nvn/dmsh_abi_vk.h'
METAL_ONLY = {abi.KIND_GEOMETRY, abi.KIND_BORDER}   # the geometry lowering's helpers and the border colour block: no Vulkan binding


def vk_values():
    out = {}
    for m in re.finditer(r'^#define DMSH_VK_(\w+)\s+(0x[0-9A-Fa-f]+|\d+|"[^"]*")', HEADER.read_text(), re.M):
        out[m.group(1)] = m.group(2)[1:-1] if m.group(2).startswith('"') else int(m.group(2), 0)
    return out


class Abi(unittest.TestCase):
    def test_struct_sizes(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('no C compiler')
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / 't.c'; src.write_text(f'#include "{HEADER}"\nint main(void) {{ return 0; }}\n')
            r = subprocess.run([cc, '-std=c11', '-c', str(src), '-o', str(Path(d) / 't.o')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_pack_formats(self):
        v = vk_values()
        self.assertEqual(struct.calcsize('<II16s16s16s16sIIIIIIQQHH12s'), v['HEADER_SIZE'])
        self.assertEqual(struct.calcsize('<QQBBHIIHH3HBBIIII8x'), v['ENTRY_SIZE'])
        self.assertEqual(struct.calcsize('<BBHBBHhHBBhHHI'), v['BIND_SIZE'])
        self.assertEqual(struct.calcsize('<IIIBBH'), v['MOD_SIZE'])
        # the header, entry and module layouts are vk_android_delta.py's (the build and the delta update read and write them alike), the binding record the build's
        src = (TOOLS / 'vk_android_build.py').read_text() + (TOOLS / 'vk_android_delta.py').read_text()
        for fmt in ("'<II16s16s16s16sIIIIIIQQHH12s'", "'<QQBBHIIHH3HBBIIII8x'", "'<BBHBBHhHBBhHHI'", "'<IIIBBH'"):
            self.assertIn(fmt, src, 'the build packs another layout than this test checks')

    def test_every_kind_numbered(self):
        v = vk_values()
        names = {k[len('KIND_'):] for k in v if k.startswith('KIND_')}
        for k, s in abi.VALUES.items():
            if k.startswith('KIND_') and s not in METAL_ONLY:
                self.assertIn(k[len('KIND_'):], names, f'DMSH_{k} has no DMSH_VK_{k}')
        nums = [n for k, n in v.items() if k.startswith('KIND_')]
        self.assertEqual(len(nums), len(set(nums)), 'two kinds share a number')

    def test_stage_ranges(self):
        v = vk_values()
        self.assertLessEqual(max(v['STAGE_VS'], v['STAGE_TCS'], v['STAGE_TES'], v['STAGE_GS'], v['STAGE_FS']) * v['STAGE_BINDINGS'] + v['STAGE_BINDINGS'], 0x10000)

    def test_version_header_current(self):
        r = subprocess.run([sys.executable, str(TOOLS / 'vk_android_build.py'), '--version'], capture_output=True, text=True, check=True)
        self.assertIn(f'"{r.stdout.strip()}"', (REPO / 'dm/gfx/nvn/dmsh_chain_vk.h').read_text(),
                      'dm/gfx/nvn/dmsh_chain_vk.h is stale: run dm/gfx/tools/vk_android_build.py --header and commit it')

    def test_same_rows(self):
        """vk_android_same.tsv: each row a tools digest, the earlier library version it was shown to reproduce, and the evidence; a version is never itself a
        digest with a row (no chains: the row names the version the libraries carry), and --version is the row's version when the tree's digest has one"""
        rows = [l.split('\t') for l in (TOOLS / 'vk_android_same.tsv').read_text().splitlines() if l and not l.startswith('#')]
        digests = [r[0] for r in rows]
        self.assertEqual(len(digests), len(set(digests)), 'a tools digest has two rows')
        for r in rows:
            self.assertEqual(len(r), 3, f'{r}: tools digest, library version, evidence')
            self.assertRegex(r[0], r'^[0-9a-f]{12}$'); self.assertRegex(r[1], r'^[0-9a-f]{12}$'); self.assertTrue(r[2].strip(), f'{r[0]}: no evidence')
            self.assertNotIn(r[1], digests, f'{r[1]} is itself a digest with a row: name the version its libraries carry')
        run = lambda a: subprocess.run([sys.executable, str(TOOLS / 'vk_android_build.py'), a], capture_output=True, text=True, check=True).stdout.strip()
        d = run('--digest')
        self.assertEqual(run('--version'), dict((r[0], r[1]) for r in rows).get(d, d))


# A vertex shader as the translator writes one: a position in gl_PerVertex, one vec4 output at location 1, FMax of two floats, no GLSL import, an unused Float64 capability
VS = '''OpCapability Shader
OpCapability Float64
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pv %o1 %i0
OpDecorate %blk Block
OpMemberDecorate %blk 0 BuiltIn Position
OpDecorate %o1 Location 1
OpDecorate %i0 Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%v4 = OpTypeVector %f32 4
%blk = OpTypeStruct %v4
%pblk = OpTypePointer Output %blk
%pv = OpVariable %pblk Output
%pv4 = OpTypePointer Output %v4
%o1 = OpVariable %pv4 Output
%pi4 = OpTypePointer Input %v4
%i0 = OpVariable %pi4 Input
%int = OpTypeInt 32 1
%c0 = OpConstant %int 0
%main = OpFunction %void None %fn
%l = OpLabel
%x = OpLoad %v4 %i0
%p = OpAccessChain %pv4 %pv %c0
OpStore %p %x
OpReturn
OpFunctionEnd
'''


# A pixel shader as the translator writes one: Ryujinx's support buffer (a uint first), colour 0 written from a float, colour 1 component 0 from integer bits, read back once
FS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %o0 %o1
OpExecutionMode %main OriginUpperLeft
OpName %sb "support_buffer"
OpDecorate %sbt Block
OpMemberDecorate %sbt 0 Offset 0
OpMemberDecorate %sbt 1 Offset 16
OpDecorate %sb DescriptorSet 0
OpDecorate %sb Binding 0
OpDecorate %o0 Location 0
OpDecorate %o1 Location 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%u32 = OpTypeInt 32 0
%int = OpTypeInt 32 1
%v4 = OpTypeVector %f32 4
%sbt = OpTypeStruct %u32 %v4
%psb = OpTypePointer Uniform %sbt
%sb = OpVariable %psb Uniform
%pv4 = OpTypePointer Output %v4
%pf = OpTypePointer Output %f32
%o0 = OpVariable %pv4 Output
%o1 = OpVariable %pv4 Output
%c0 = OpConstant %int 0
%u7 = OpConstant %u32 7
%f1 = OpConstant %f32 1
%one = OpConstantComposite %v4 %f1 %f1 %f1 %f1
%main = OpFunction %void None %fn
%l = OpLabel
OpStore %o0 %one
%bits = OpBitcast %f32 %u7
%p = OpAccessChain %pf %o1 %c0
OpStore %p %bits
%back = OpLoad %f32 %p
OpStore %p %back
OpReturn
OpFunctionEnd
'''

# A pixel shader drawn with a mesh program: flat input 0 (written per primitive), input 1 per vertex, a storage buffer read
MESH_FS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %i0 %i1 %o0
OpExecutionMode %main OriginUpperLeft
OpDecorate %i0 Location 0
OpDecorate %i0 Flat
OpDecorate %i1 Location 1
OpDecorate %o0 Location 0
OpDecorate %ra ArrayStride 4
OpDecorate %sbt BufferBlock
OpMemberDecorate %sbt 0 Offset 0
OpDecorate %sb DescriptorSet 0
OpDecorate %sb Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%int = OpTypeInt 32 1
%v4 = OpTypeVector %f32 4
%ra = OpTypeRuntimeArray %f32
%sbt = OpTypeStruct %ra
%psb = OpTypePointer Uniform %sbt
%pf = OpTypePointer Uniform %f32
%sb = OpVariable %psb Uniform
%pin = OpTypePointer Input %v4
%pout = OpTypePointer Output %v4
%i0 = OpVariable %pin Input
%i1 = OpVariable %pin Input
%o0 = OpVariable %pout Output
%c0 = OpConstant %int 0
%main = OpFunction %void None %fn
%l = OpLabel
%a = OpLoad %v4 %i0
%b = OpLoad %v4 %i1
%p = OpAccessChain %pf %sb %c0 %c0
%s = OpLoad %f32 %p
%ab = OpFAdd %v4 %a %b
%v = OpVectorTimesScalar %v4 %ab %s
OpStore %o0 %v
OpReturn
OpFunctionEnd
'''

# A passthrough geometry shader as Ryujinx emulates it: the viewport mask output set to all, then a primitive emitted only when it is not 0
SCALE_CS = '''OpCapability Shader
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpName %sup "support_buffer"
OpDecorate %arr ArrayStride 16
OpMemberDecorate %SB 0 Offset 0
OpMemberDecorate %SB 1 Offset 16
OpMemberDecorate %SB 2 Offset 32
OpMemberDecorate %SB 3 Offset 48
OpMemberDecorate %SB 4 Offset 64
OpMemberDecorate %SB 5 Offset 80
OpDecorate %SB Block
OpDecorate %sup DescriptorSet 0
OpDecorate %sup Binding 0
%void = OpTypeVoid
%fv = OpTypeFunction %void
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%bool = OpTypeBool
%v4f = OpTypeVector %float 4
%int_73 = OpConstant %int 73
%arr = OpTypeArray %v4f %int_73
%SB = OpTypeStruct %v4f %v4f %v4f %v4f %v4f %arr
%ptr_SB = OpTypePointer Uniform %SB
%sup = OpVariable %ptr_SB Uniform
%ptr_f = OpTypePointer Uniform %float
%fi2 = OpTypeFunction %int %int %int
%int_0 = OpConstant %int 0
%int_1 = OpConstant %int 1
%int_4 = OpConstant %int 4
%int_5 = OpConstant %int 5
%int_7 = OpConstant %int 7
%float_1 = OpConstant %float 1
%main = OpFunction %void None %fv
%m0 = OpLabel
%a = OpFunctionCall %int %fetch %int_7 %int_0
%b = OpFunctionCall %int %fetch %a %int_1
%c = OpFunctionCall %int %unscale %b %int_0
%d = OpFunctionCall %int %other %c %int_0
OpReturn
OpFunctionEnd
%fetch = OpFunction %int None %fi2
%fx = OpFunctionParameter %int
%fs = OpFunctionParameter %int
%f0 = OpLabel
%fidx = OpIAdd %int %int_1 %fs
%fp = OpAccessChain %ptr_f %sup %int_5 %fidx %int_0
%fsc = OpLoad %float %fp
%fone = OpFOrdEqual %bool %fsc %float_1
OpSelectionMerge %f2 None
OpBranchConditional %fone %f1 %f2
%f1 = OpLabel
OpReturnValue %fx
%f2 = OpLabel
%fcv = OpConvertSToF %float %fx
%fmul = OpFMul %float %fcv %fsc
%fr = OpConvertFToS %int %fmul
OpReturnValue %fr
OpFunctionEnd
%unscale = OpFunction %int None %fi2
%ux = OpFunctionParameter %int
%us = OpFunctionParameter %int
%u0 = OpLabel
%uidx = OpIAdd %int %int_1 %us
%up = OpAccessChain %ptr_f %sup %int_5 %uidx %int_0
%ul = OpLoad %float %up
%usc = OpExtInst %float %glsl FAbs %ul
%uone = OpFOrdEqual %bool %usc %float_1
OpSelectionMerge %u2 None
OpBranchConditional %uone %u1 %u2
%u1 = OpLabel
OpReturnValue %ux
%u2 = OpLabel
%ucv = OpConvertSToF %float %ux
%udiv = OpFDiv %float %ucv %usc
%ur = OpConvertFToS %int %udiv
OpReturnValue %ur
OpFunctionEnd
%other = OpFunction %int None %fi2
%ox = OpFunctionParameter %int
%os = OpFunctionParameter %int
%o0 = OpLabel
%op = OpAccessChain %ptr_f %sup %int_4 %int_0
%osc = OpLoad %float %op
%oone = OpFOrdEqual %bool %osc %float_1
OpSelectionMerge %o2 None
OpBranchConditional %oone %o1 %o2
%o1 = OpLabel
OpReturnValue %ox
%o2 = OpLabel
OpReturnValue %os
OpFunctionEnd
'''

GS = '''OpCapability Geometry
OpCapability ShaderViewportMaskNV
OpExtension "SPV_NV_viewport_array2"
OpMemoryModel Logical GLSL450
OpEntryPoint Geometry %main "main" %vm
OpExecutionMode %main Triangles
OpExecutionMode %main Invocations 1
OpExecutionMode %main OutputTriangleStrip
OpExecutionMode %main OutputVertices 3
OpDecorate %vm BuiltIn ViewportMaskNV
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%u32 = OpTypeInt 32 0
%bool = OpTypeBool
%u1 = OpConstant %u32 1
%arr = OpTypeArray %int %u1
%parr = OpTypePointer Output %arr
%pi = OpTypePointer Output %int
%vm = OpVariable %parr Output
%c0 = OpConstant %int 0
%m1 = OpConstant %int -1
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %pi %vm %c0
OpStore %p %m1
%v = OpLoad %int %p
%z = OpIEqual %bool %v %c0
OpSelectionMerge %end None
OpBranchConditional %z %end %emit
%emit = OpLabel
OpEmitVertex
OpBranch %end
%end = OpLabel
OpReturn
OpFunctionEnd
'''


# A compute shader as the translator writes a 1D integer fetch of a 2D slot: (x & 0x3FFF, x >> 14) through Ryujinx's render-scale helper, then OpImageFetch with Lod 0.
# "rows" is read only so (row_fetch_buffers makes it a texel buffer); "sized" is also size-queried (left as it is)
ROWS_CS = '''OpCapability Shader
OpCapability ImageQuery
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpName %rows "rows"
OpName %sized "sized"
OpName %x "x"
OpName %i1 "i1"
OpName %s1 "s1"
OpName %xa "xa"
OpName %twice "twice"
OpName %texel0 "texel0"
OpName %s4 "s4"
OpDecorate %rows DescriptorSet 0
OpDecorate %rows Binding 1
OpDecorate %sized DescriptorSet 0
OpDecorate %sized Binding 2
OpDecorate %twice DescriptorSet 0
OpDecorate %twice Binding 4
OpDecorate %texel0 DescriptorSet 0
OpDecorate %texel0 Binding 5
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 3
OpDecorate %ob Block
OpMemberDecorate %ob 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v2int = OpTypeVector %int 2
%v4float = OpTypeVector %float 4
%img = OpTypeImage %float 2D 0 0 0 1 Unknown
%simg = OpTypeSampledImage %img
%pimg = OpTypePointer UniformConstant %simg
%ob = OpTypeStruct %v4float
%pob = OpTypePointer StorageBuffer %ob
%pv4 = OpTypePointer StorageBuffer %v4float
%sfn = OpTypeFunction %int %int %int
%pfi = OpTypePointer Function %int
%c16383 = OpConstant %int 16383
%c14 = OpConstant %int 14
%c0 = OpConstant %int 0
%c1 = OpConstant %int 1
%c70000 = OpConstant %int 70000
%rows = OpVariable %pimg UniformConstant
%sized = OpVariable %pimg UniformConstant
%twice = OpVariable %pimg UniformConstant
%texel0 = OpVariable %pimg UniformConstant
%out = OpVariable %pob StorageBuffer
%scale = OpFunction %int None %sfn
%a = OpFunctionParameter %int
%b = OpFunctionParameter %int
%sl = OpLabel
OpReturnValue %a
OpFunctionEnd
%main = OpFunction %void None %fn
%l = OpLabel
%lv = OpVariable %pfi Function
%x = OpCopyObject %int %c70000
%xu = OpBitcast %uint %x
%xa = OpBitcast %int %xu
%xb = OpBitcast %int %xu
%lo = OpBitwiseAnd %int %xa %c16383
%hi = OpShiftRightLogical %int %xb %c14
%hu = OpBitcast %uint %hi
%hs = OpBitcast %int %hu
%cx = OpFunctionCall %int %scale %lo %c0
%cy = OpFunctionCall %int %scale %hs %c0
%co = OpCompositeConstruct %v2int %cx %cy
%s1 = OpLoad %simg %rows
%i1 = OpImage %img %s1
%f1 = OpImageFetch %v4float %i1 %co Lod %c0
%s2 = OpLoad %simg %sized
%i2 = OpImage %img %s2
%q = OpImageQuerySizeLod %v2int %i2 %c0
%f2 = OpImageFetch %v4float %i2 %co Lod %c0
%ya = OpIAdd %int %c70000 %c0
%yb = OpIAdd %int %c70000 %c0
%l2 = OpBitwiseAnd %int %ya %c16383
OpStore %lv %l2
%l2r = OpLoad %int %lv
%h2 = OpShiftRightLogical %int %yb %c14
%cx2 = OpFunctionCall %int %scale %l2r %c0
%cy2 = OpFunctionCall %int %scale %h2 %c0
%co2 = OpCompositeConstruct %v2int %cx2 %cy2
%s3 = OpLoad %simg %twice
%i3 = OpImage %img %s3
%f3 = OpImageFetch %v4float %i3 %co2 Lod %c0
%k0 = OpFunctionCall %int %scale %c0 %c0
%k1 = OpFunctionCall %int %scale %c1 %c0
%kc0 = OpCompositeConstruct %v2int %k0 %k0
%kc1 = OpCompositeConstruct %v2int %k1 %k0
%s4 = OpLoad %simg %rows
%i4 = OpImage %img %s4
%f4 = OpImageFetch %v4float %i4 %kc0 Lod %c0
%f5 = OpImageFetch %v4float %i4 %kc1 Lod %c0
%s6 = OpLoad %simg %texel0
%i6 = OpImage %img %s6
%f6 = OpImageFetch %v4float %i6 %kc0 Lod %c0
%sa = OpFAdd %v4float %f1 %f3
%sb = OpFAdd %v4float %sa %f4
%sc = OpFAdd %v4float %sb %f5
%sum = OpFAdd %v4float %sc %f6
%po = OpAccessChain %pv4 %out %c0
OpStore %po %sum
OpReturn
OpFunctionEnd
'''


QUERY_CS = '''OpCapability Shader
OpCapability ImageQuery
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpName %sz "sz"
OpName %smp "smp"
OpDecorate %sz DescriptorSet 0
OpDecorate %sz Binding 1
OpDecorate %smp DescriptorSet 0
OpDecorate %smp Binding 2
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 3
OpDecorate %ob Block
OpMemberDecorate %ob 0 Offset 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v2int = OpTypeVector %int 2
%v4float = OpTypeVector %float 4
%img = OpTypeImage %float 2D 0 0 0 1 Unknown
%simg = OpTypeSampledImage %img
%pimg = OpTypePointer UniformConstant %simg
%ob = OpTypeStruct %int
%pob = OpTypePointer StorageBuffer %ob
%pi = OpTypePointer StorageBuffer %int
%c0 = OpConstant %int 0
%sz = OpVariable %pimg UniformConstant
%smp = OpVariable %pimg UniformConstant
%out = OpVariable %pob StorageBuffer
%main = OpFunction %void None %fn
%l = OpLabel
%s1 = OpLoad %simg %sz
%i1 = OpImage %img %s1
%q1 = OpImageQuerySizeLod %v2int %i1 %c0
%h = OpCompositeExtract %int %q1 1
%s2 = OpLoad %simg %smp
%i2 = OpImage %img %s2
%q2 = OpImageQuerySizeLod %v2int %i2 %c0
%w = OpCompositeExtract %int %q2 0
%cv = OpCompositeConstruct %v2int %w %h
%f = OpImageFetch %v4float %i2 %cv Lod %c0
%po = OpAccessChain %pi %out %c0
%sum = OpIAdd %int %h %w
OpStore %po %sum
OpReturn
OpFunctionEnd
'''


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'SPIRV-Tools not installed')
class Rewrites(unittest.TestCase):
    def module(self, text):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'a.spvasm').write_text(text)
            subprocess.run(['spirv-as', '--target-env', 'spv1.3', str(Path(d) / 'a.spvasm'), '-o', str(Path(d) / 'a.spv')], check=True)
            return spv.Module((Path(d) / "a.spv").read_bytes())   # SPIR-V 1.3, as the translator writes

    def valid(self, m):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'b.spv'; p.write_bytes(m.data())
            r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(p)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            return subprocess.run(['spirv-dis', str(p)], capture_output=True, text=True).stdout if shutil.which('spirv-dis') else ''

    def test_row_fetch_buffers(self):
        """A slot read only as rows of 16384 texels (each half from its own bitcast of x, as Orbitals' f2a89c6cbf046098 has) becomes a uniform texel buffer read at x
        with no OpImage left (the runtime retyper follows a load straight to its reads); one whose halves come from two loads of x, the low one through a local
        (f1b3c237bfd5b871), read at hi << 14 | lo; one whose x the translator folded to a constant, beside row reads, read at that element; one read only at a constant texel, or also
        size-queried, stays a 2D slot"""
        # after scale_identity (which runs first) the helper's calls are copies of their coordinate: the same two slots are rewritten (none were in the v0.3.50 merge)
        self.assertEqual(spv.row_fetch_buffers(self.module(re.sub(r'OpFunctionCall %int %scale (%\w+) %c0', r'OpCopyObject %int \1', ROWS_CS))), 2)
        m = self.module(ROWS_CS)
        self.assertEqual(spv.row_fetch_buffers(m), 2)
        r = {e['name']: e for e in m.resources()}
        self.assertEqual((r['rows']['desc'], r['rows']['dim']['dim']), (spv.VK_UNIFORM_TEXEL, spv.DIM_BUFFER))
        self.assertEqual((r['twice']['desc'], r['twice']['dim']['dim']), (spv.VK_UNIFORM_TEXEL, spv.DIM_BUFFER))
        self.assertEqual((r['texel0']['desc'], r['texel0']['dim']['dim']), (spv.VK_COMBINED, 1))   # read only at a constant texel: may be a 2D texture
        self.assertEqual((r['sized']['desc'], r['sized']['dim']['dim']), (spv.VK_COMBINED, 1))
        text = self.valid(m)
        if text:
            self.assertIn('SampledBuffer', text)
            self.assertIn('OpImageFetch %v4float %s1 %xa', text)   # x itself, from the load, no Lod
            self.assertNotIn('%i1', text)
            self.assertIn('OpImageFetch %v4float %s4 %int_0', text)   # rows' folded constant reads: elements 0 and 1
            self.assertIn('OpImageFetch %v4float %s4 %int_1', text)
            self.assertRegex(text, r'(%\w+) = OpShiftLeftLogical %int %\w+ %int_14\n +(%\w+) = OpBitwiseOr %int %\w+ \1\n +%\w+ = OpImageFetch %v4float %\w+ \2\n')   # twice: hi << 14 | lo

    def test_query_dim3(self):
        """A 2D slot read only by size queries whose x and y are used becomes 3D when the build names it (its handle bound as 3D elsewhere in the shader, Orbitals'
        cea444eb38c3c1db); one also fetched stays 2D"""
        m = self.module(QUERY_CS)
        ids = {r['name']: r['id'] for r in m.resources()}
        self.assertEqual(spv.query_dim3(m, [ids['sz'], ids['smp']]), 1)
        r = {e['name']: e for e in m.resources()}
        self.assertEqual((r['sz']['dim']['dim'], r['smp']['dim']['dim']), (2, 1))
        text = self.valid(m)
        if text: self.assertIn('OpImageQuerySizeLod %v3int', text)

    def test_vertex_rewrites(self):
        m = self.module(VS)
        self.assertEqual(spv.strip_unused_caps(m), ['Float64'])
        self.assertEqual(spv.init_outputs(m, {1, 3}), [3])   # location 1 exists, 3 is added
        self.assertTrue(spv.gl_depth(m, 0))
        text = self.valid(m)
        if text:
            self.assertIn('Fma', text)
            self.assertIn('SpecId 0', text)
            self.assertIn('Location 3', text)
            self.assertNotIn('Float64', text)

    def test_precise(self):
        text = VS.replace('OpMemoryModel', '%glsl = OpExtInstImport "GLSL.std.450"\nOpMemoryModel').replace('OpStore %p %x', '%y = OpExtInst %v4 %glsl FMax %x %x\nOpStore %p %y')
        m = self.module(text)
        self.assertEqual(spv.precise_minmax(m), 1)
        out = self.valid(m)
        if out: self.assertIn('NMax', out)

    def test_fragment_variants(self):
        m = self.module(FS)
        self.assertEqual(spv.int_outputs(m), {1})
        self.assertEqual(spv.const_written_outputs(m), set())
        self.assertTrue(spv.alpha_test(m, 1))
        text = self.valid(m)
        if text: self.assertIn('SpecId 1', text); self.assertIn('OpKill', text)
        u = spv.Module(m.data()); self.assertEqual(spv.retype_outputs(u, {0, 1}), [0, 1])
        text = self.valid(u)
        if text: self.assertNotIn('OpVariable %_ptr_Output_v4float', text)
        mx = spv.Module(m.data()); self.assertEqual(spv.retype_outputs(mx, {1}), [1])
        text = self.valid(mx)
        if text: self.assertIn('OpVariable %_ptr_Output_v4uint', text); self.assertIn('OpVariable %_ptr_Output_v4float', text)
        d = spv.Module(m.data()); self.assertTrue(spv.dual_source(d))
        text = self.valid(d)
        if text: self.assertIn('Index 1', text)
        self.assertEqual(spv.const_written_outputs(self.module(FS.replace('OpStore %p %bits', 'OpStore %p %f1').replace('OpStore %p %back', ''))), {0, 1})

    def test_per_primitive_inputs(self):
        # a pixel shader of a mesh program: input 0 per primitive; its storage buffer (Uniform + BufferBlock in SPIR-V 1.3) becomes StorageBuffer + Block in 1.4
        m = self.module(MESH_FS)
        self.assertEqual(spv.per_primitive_inputs(m, {0, 5}), [0])
        self.assertEqual(m.header[1], 0x10400)
        self.assertIn(spv.CAP_MESH_SHADING, m.caps)
        self.assertEqual(m.exts, ['SPV_EXT_mesh_shader'])
        text = self.valid(m)
        if text:
            self.assertIn('PerPrimitiveEXT', text); self.assertIn('StorageBuffer', text); self.assertNotIn('BufferBlock', text)
        self.assertEqual(spv.per_primitive_inputs(self.module(MESH_FS), {3}), [])   # no such input: unchanged

    def test_scale_identity(self):
        # the translator's TexelFetchScale (compute form) and TextureSizeUnscale (|scale|) called from main, and a look-alike that compares another support member
        m = self.module(SCALE_CS)
        self.assertEqual(spv.scale_identity(m), 3)   # two fetch-scale calls and one unscale call; the look-alike's call stays
        text = self.valid(m)
        calls = [x for x in m.ins if x[0] & 0xffff == spv.OP_FUNCTION_CALL]
        self.assertEqual(len(calls), 1)
        self.assertEqual(sum(1 for x in m.ins if x[0] & 0xffff == spv.OP_FUNCTION), 2)   # main and the look-alike: the helpers without callers removed
        if text: self.assertEqual(text.count('OpCopyObject'), 3)
        self.assertEqual(spv.scale_identity(m), 0)   # nothing left to replace
        self.assertEqual(spv.scale_identity(self.module(SCALE_CS.replace('OpName %sup "support_buffer"', ''))), 0)   # no support buffer: untouched

    def test_viewport_mask_private(self):
        m = self.module(GS)
        self.assertTrue(spv.private_viewport_mask(m))
        text = self.valid(m)
        self.assertNotIn(5255, m.caps)
        self.assertEqual(m.exts, [])
        if text: self.assertIn('Private', text); self.assertNotIn('ViewportMaskNV', text)


class Dominance(unittest.TestCase):
    def test_same_as_set_fixed_point(self):
        """promote_locals' dominance test (linear memory) answers as the set fixed point it replaced, on random graphs with unreachable cycles, self loops and
        branches back to the entry"""
        import random
        def sets(blks, preds):
            dom = {b: set(blks) for b in blks}; dom[blks[0]] = {blks[0]}
            chg = True
            while chg:
                chg = False
                for b in blks[1:]:
                    d = ({b} | set.intersection(*(dom[p] for p in preds[b]))) if preds[b] else {b}
                    if d != dom[b]: dom[b] = d; chg = True
            return dom
        rnd = random.Random(1)
        for _ in range(3000):
            n = rnd.randint(1, 10); blks = rnd.sample(range(100, 200), n)
            preds = {b: set() for b in blks}
            for _ in range(rnd.randint(0, 3 * n)): preds[rnd.choice(blks)].add(rnd.choice(blks))
            d, f = sets(blks, preds), spv._dominance(blks, preds)
            for a in blks:
                for b in blks: self.assertEqual(a in d[b], f(a, b), (blks, preds, a, b))


if __name__ == '__main__':
    unittest.main()
