"""Texture arrays of uniform-buffer handles read through the pools (vk_android_spv.py ubo_handle_tables): the translator declares Ryujinx's texture array from a game uniform buffer
with one element per handle 64 KiB holds (8,192), which the replay cannot bind element by element. Each access chain into the array and its load become a load of the handle word
from the uniform buffer's block, its texture ID indexing the texture pool and its sampler ID the sampler pool, joined by OpSampledImage before each use. Modules are assembled with
spirv-as and checked with spirv-val and the tool's own reflection (resources), and the rewritten words are run on a few handles by a small interpreter of the generated code."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[0] / 'tools'))
import vk_android_spv as spv  # noqa: E402

# A pixel shader as the translator writes it: its own block for c[12] (float rows), a combined cube array of 8,192 handles from it, indexed by a computed handle position, the
# sample in another block than the load
COMBINED = '''
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %out %arr
               OpExecutionMode %main OriginUpperLeft
               OpName %fp_c12 "fp_c12"
               OpName %arr "fp_tcube_cb12_0"
               OpDecorate %out Location 0
               OpDecorate %rows ArrayStride 16
               OpDecorate %blk Block
               OpMemberDecorate %blk 0 Offset 0
               OpDecorate %fp_c12 DescriptorSet 0
               OpDecorate %fp_c12 Binding 2
               OpDecorate %arr DescriptorSet 0
               OpDecorate %arr Binding 5
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
        %int = OpTypeInt 32 1
       %uint = OpTypeInt 32 0
       %bool = OpTypeBool
        %v4f = OpTypeVector %float 4
        %v3f = OpTypeVector %float 3
      %u4096 = OpConstant %uint 4096
      %u8192 = OpConstant %uint 8192
       %rows = OpTypeArray %v4f %u4096
        %blk = OpTypeStruct %rows
       %pblk = OpTypePointer Uniform %blk
        %img = OpTypeImage %float Cube 0 0 0 1 Unknown
         %si = OpTypeSampledImage %img
       %sarr = OpTypeArray %si %u8192
      %psarr = OpTypePointer UniformConstant %sarr
        %psi = OpTypePointer UniformConstant %si
       %pout = OpTypePointer Output %v4f
         %i5 = OpConstant %int 5
        %one = OpConstant %float 1
       %true = OpConstantTrue %bool
      %coord = OpConstantComposite %v3f %one %one %one
     %fp_c12 = OpVariable %pblk Uniform
        %arr = OpVariable %psarr UniformConstant
        %out = OpVariable %pout Output
       %main = OpFunction %void None %fn
          %l = OpLabel
          %p = OpAccessChain %psi %arr %i5
          %s = OpLoad %si %p
               OpSelectionMerge %m None
               OpBranchConditional %true %t %m
          %t = OpLabel
          %c = OpImageSampleImplicitLod %v4f %s %coord
               OpStore %out %c
               OpBranch %m
          %m = OpLabel
               OpReturn
               OpFunctionEnd
'''
# no block of the slot's own (the shader reads nothing else of c[10]), a 2D array with a constant sampler (handle type 3), element 0 at byte 0x40
CONST_SAMPLER = COMBINED.replace('OpName %fp_c12 "fp_c12"\n', '').replace('"fp_tcube_cb12_0"', '"fp_t2d_cb10_30010010"').replace('Cube 0 0 0 1', '2D 0 0 0 1') \
    .replace('%coord = OpConstantComposite %v3f %one %one %one', '%v2f = OpTypeVector %float 2\n      %coord = OpConstantComposite %v2f %one %one')
# the array reaches a function call: left as it is
BAD = COMBINED.replace('%pout = OpTypePointer Output %v4f', '%pout = OpTypePointer Output %v4f\n       %fn2 = OpTypeFunction %v4f %si') \
    .replace('''          %c = OpImageSampleImplicitLod %v4f %s %coord''', '''          %c = OpFunctionCall %v4f %f %s''') + '''
          %f = OpFunction %v4f None %fn2
         %fp = OpFunctionParameter %si
         %l2 = OpLabel
         %r2 = OpImageSampleImplicitLod %v4f %fp %coord
               OpReturnValue %r2
               OpFunctionEnd
'''


# the array's id is 80, which an extended instruction holds as its number (GLSL.std.450 NMax): a literal, not a use of the array (Welcome Tour's 58 shaders left as they were)
LITERAL = COMBINED.replace('%arr', '%80').replace('               OpMemoryModel', '       %glsl = OpExtInstImport "GLSL.std.450"\n               OpMemoryModel') \
    .replace('          %s = OpLoad %si %p', '          %s = OpLoad %si %p\n         %nm = OpExtInst %float %glsl NMax %one %one')


def assemble(d, text):
    (d / 'a.spvasm').write_text(text)
    subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', '--preserve-numeric-ids', str(d / 'a.spvasm'), '-o', str(d / 'a.spv')], check=True)
    return spv.Module((d / 'a.spv').read_bytes())


def validate(t, d, m):
    (d / 'b.spv').write_bytes(m.data())
    v = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(d / 'b.spv')], capture_output=True, text=True)
    t.assertEqual(v.returncode, 0, v.stdout + v.stderr)


def by_name(m):
    return {r['name']: r for r in m.resources()}


def run_handle(m, words):
    """The texture and sampler IDs the rewritten code reads, with the uniform buffers' 32-bit words given ({(variable name, word index): value}): the instructions from the
    first OpShiftLeftLogical to the last OpAccessChain into a pool, interpreted (they are integer arithmetic, uniform loads and pool access chains only)"""
    names = {v: n for v, n in m.names.items()}
    val = {i: v[0] for i, (t, v) in m.consts.items() if len(v) == 1}
    ptrs, out = {}, {}
    for x in m.ins:
        op = x[0] & 0xffff
        g = lambda i: val[x[i]]
        if op == 196: val[x[2]] = (g(3) << g(4)) & 0xffffffff
        elif op == 194: val[x[2]] = g(3) >> g(4)
        elif op == 199: val[x[2]] = g(3) & g(4)
        elif op == 197: val[x[2]] = g(3) | g(4)
        elif op == 128: val[x[2]] = (g(3) + g(4)) & 0xffffffff
        elif op == spv.OP_BITCAST and x[3] in val: val[x[2]] = val[x[3]]
        elif op == spv.OP_ACCESS_CHAIN and names.get(x[3], '').endswith(spv.POOL_VAR_SUFFIX): out[names[x[3]][3]] = g(4)
        elif op == spv.OP_ACCESS_CHAIN and len(x) == 7: ptrs[x[2]] = words[names[x[3]], g(5) * 4 + g(6)]
        elif op == spv.OP_LOAD and x[3] in ptrs: val[x[2]] = ptrs[x[3]]
    return out


@unittest.skipUnless(shutil.which('spirv-as') and shutil.which('spirv-val'), 'needs SPIRV-Tools')
class UboHandleTables(unittest.TestCase):
    def test_combined_reads_the_pools(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); m = assemble(d, COMBINED)
            arr = by_name(m)['fp_tcube_cb12_0']
            self.assertEqual((arr['desc'], arr['count']), (spv.VK_COMBINED, 8192))
            self.assertEqual(spv.ubo_handle_tables(m, {arr['id']: {'prefix': 'fp', 'slot': 9, 'off': 0, 'samp': None}}), 1)
            r = by_name(m)
            self.assertNotIn('fp_tcube_cb12_0', r)
            pools = sorted((x['desc'], x['count']) for n, x in r.items() if n.endswith(spv.POOL_VAR_SUFFIX))
            self.assertEqual(pools, [(spv.VK_SAMPLER, 4096), (spv.VK_SAMPLED_IMAGE, 1 << 20)])
            self.assertEqual([n for n in r if n.startswith('fp_c')], ['fp_c12'])   # the translator's block, read again: no second one
            ops = [x[0] & 0xffff for x in m.ins]
            k_si, k_smp = ops.index(spv.OP_SAMPLED_IMAGE), next(k for k, x in enumerate(m.ins) if x[0] & 0xffff == 87)
            self.assertEqual(k_smp, k_si + 1)   # made right before its use, in the use's block
            validate(self, d, m)
            # element 5: word 10 of c[12]; texture ID 0x12345, sampler ID 0x7b
            self.assertEqual(run_handle(m, {('fp_c12', 10): 0x7b << 20 | 0x12345}), {'t': 0x12345, 's': 0x7b})
            self.assertEqual(spv.ubo_handle_tables(m, {}), 0)

    def test_constant_sampler_and_new_block(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); m = assemble(d, CONST_SAMPLER)
            arr = by_name(m)['fp_t2d_cb10_30010010']
            self.assertEqual(spv.ubo_handle_tables(m, {arr['id']: {'prefix': 'fp', 'slot': 7, 'off': 0x40, 'samp': ('id', 0x104)}}), 1)
            r = by_name(m)
            self.assertEqual((r['fp_c10']['desc'], r['fp_c10']['count']), (spv.VK_UNIFORM_BUFFER, 1))   # a block of its own for c[10]
            validate(self, d, m)
            # element 5 at byte 0x40 + 40: word 26
            self.assertEqual(run_handle(m, {('fp_c10', 26): 0x99 << 20 | 0x321}), {'t': 0x321, 's': 0x104})

    def test_sampler_word_ored_in(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); m = assemble(d, COMBINED)
            arr = by_name(m)['fp_tcube_cb12_0']
            self.assertEqual(spv.ubo_handle_tables(m, {arr['id']: {'prefix': 'fp', 'slot': 9, 'off': 0, 'samp': (2, 0x18)}}), 1)
            validate(self, d, m)
            self.assertEqual(run_handle(m, {('fp_c12', 10): 0x2a, ('fp_c5', 6): 0x55 << 20}), {'t': 0x2a, 's': 0x55})

    def test_a_literal_equal_to_the_array_id_is_no_use(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d); m = assemble(d, LITERAL)
            arr = by_name(m)['fp_tcube_cb12_0']
            self.assertEqual(arr['id'], 80)
            self.assertTrue(any(x[0] & 0xffff == spv.OP_EXT_INST and x[4] == 80 for x in m.ins))
            self.assertEqual(spv.ubo_handle_tables(m, {arr['id']: {'prefix': 'fp', 'slot': 9, 'off': 0, 'samp': None}}), 1)
            self.assertTrue(any(x[0] & 0xffff == spv.OP_EXT_INST and x[4] == 80 for x in m.ins))   # the literal is untouched
            validate(self, d, m)

    def test_other_uses_are_left(self):
        with tempfile.TemporaryDirectory() as d:
            m = assemble(Path(d), BAD)
            arr = by_name(m)['fp_tcube_cb12_0']
            self.assertEqual(spv.ubo_handle_tables(m, {arr['id']: {'prefix': 'fp', 'slot': 9, 'off': 0, 'samp': None}}), 0)
            self.assertIn('fp_tcube_cb12_0', by_name(m))


if __name__ == '__main__':
    unittest.main()
