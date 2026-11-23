#!/usr/bin/env python3
"""Ampere ALU instructions through the offline translator (dmsh_xlate, arch sm86), on registers the translator cannot fold, their result stored to shared memory so
that it is kept: BREV, and HADD2 / HMUL2 with an immediate (test_dmsh_ampere_mem.py's Pokopia class has those two on constants, a negative immediate's sign, F2FP.MERGE_C,
F2I from a half, UP2UR, MEMBAR and ERRBAR). Needs DMSH_TEST_XLATE (the translator) like the other translator tests."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_dmsh_ampere_mem import EXIT, Translated, inst

STORE = (0x000fe8000000c800, 0x0000080609008388)   # STS.U32 [R9 * 16 + 8], R6 (Mario Kart World f94552a2a2bff505): the sink, R9 the thread index


# The 64-bit address of storage buffer 0 in R8:R9, as agl's analysis shaders and Bananza's muck normal pass take theirs
ULDC_SB0 = (0x000fe20000000a00, 0x0000040000047ab9)   # ULDC.64 UR4, c[0][0x10]: storage entry 0 of an NVN2 compute c0 with one storage slot
MOV_R8_UR4, MOV_R9_UR5 = (0x000fe20008000000, 0x0000000400087c02), (0x000fe20008000000, 0x0000000500097c02)


def waiting(i): return (i[0] | 0x3f << 52, i[1])   # the instruction waits on every scoreboard (control bits 116..121): it reads what an S2R before it wrote
def s2r_tid(rd): return (0x000e260000002100, 0x0000000000007919 | rd << 16)   # S2R Rd, SR_TID.X (Mario Kart World's encoding, write barrier 0)


class Alu(Translated):
    def kept(self, *code, srcs=()):
        """code between S2R R9 (and each register of srcs) = the thread index and the store of R6"""
        pre = b''.join(inst(*s2r_tid(r)) for r in (9, *srcs))
        body = b''.join(inst(*waiting(i)) for i in code)
        return self.translate(pre + body + inst(*waiting(STORE)) + inst(*EXIT), shared=0x1000)


class Brev(Alu):
    BREV = (0x001e240000000000, 0x0000001300067301)   # BREV R6, R19 (Pokémon Pokopia 6e8db04c25a53d8c, whose destination is R3)

    def test_reverses_the_bits(self):
        self.assertIn('reverse_bits', self.kept(self.BREV, srcs=(19,)))


class HalfImmediate(Alu):
    """HADD2 / HMUL2 Rd, Ra, imm (form 2): the second source is the immediate at bits 32..63, not the register field at 64 that HFMA2's B uses in that form. Pokopia's
    blur 85446a6753300fc8 steps its loop with HADD2 R6, R6, 2.0h; reading R0 there, the loop never ended and the GPU hung"""
    HADD2 = (0x000fe20000000800, 0x4000400006067430)   # HADD2 R6, R6, 2, 2 (field 64 = R0)
    HMUL2 = (HADD2[0], HADD2[1] & ~0xfff | 0x432)      # HMUL2 R6, R6, 2, 2: the same fields (no game encoding met)

    def test_hadd2_adds_the_immediate(self):
        msl = self.kept(self.HADD2, srcs=(6,))
        self.assertIn('spvFAdd', msl)
        self.assertRegex(msl, r'\b2\.0\b')   # read from R0 (never written) it was 0, and the add folded away

    def test_hmul2_multiplies_by_the_immediate(self):
        msl = self.kept(self.HMUL2, srcs=(6,))
        self.assertIn('spvFMul', msl)
        self.assertRegex(msl, r'\b2\.0\b')


class HalfRed(Translated):
    """RED.E.ADD.F16x2.FTZ.RN (0x98e, type 4 at bits 73..75): Donkey Kong Bananza's muck normal pass (be71d8f78b52e62f, e5f7ed151822e560) adds each face normal into its vertices'
    half4 normal, two halves per word. Dropped with a log line, the normals stayed the zeros the mesh pass writes, and normalizing them made NaN: black blots in the scene.
    A compare-and-swap loop on the word (InstEmitMemory.cs EmitAtomicAddF16x2). Upstream's GPU checks of the sums (metal_kernel.py) need its Mac compile flags, which this
    tree's dmsh_chain.py does not have, so they are not taken"""
    RED = (0x000fe20000000000 | 4 << 9, 0x0000000608ff098e)   # RED.F16x2.FTZ.RN [R8], R6: (control and type, opcode and registers)
    RED_E_PT = (RED[0] | 1 << 8, RED[1] | 7 << 12)   # RED.E.F16x2.FTZ.RN [R8.64], R6, unpredicated (PT)
    LDG_R6 = (0x000fe2200c1e1900, 0x0000043f08067981)   # LDG.E R6, [R8.64 + 4]: the value from the word after the sum, so it is not folded

    def test_adds_in_a_loop(self):
        msl = self.translate(b''.join(inst(*waiting(i)) for i in (s2r_tid(6), s2r_tid(8), s2r_tid(9), self.RED)) + inst(*EXIT))
        self.assertIn('atomic_compare_exchange_weak_explicit', msl)

    def test_storage_add_is_a_swap_loop(self):   # on storage buffer 0 through its 64-bit address, as the game adds
        code = b''.join(inst(*i) for i in (ULDC_SB0, MOV_R8_UR4, MOV_R9_UR5, self.LDG_R6)) + inst(*waiting(self.RED_E_PT)) + inst(*EXIT)
        msl = self.translate(code, nvn2_cs=[0, 1, 0, 0x20, 0])
        self.assertIn('atomic_compare_exchange_weak_explicit', msl)
        self.assertNotIn('if (false)', msl)


class Vote(Alu):
    """VOTEU (0x886) writes the uniform predicate UPd and leaves the lane predicate of that number alone; VOTE (0x806) writes the lane predicate. nvdisasm reads 0x886 as
    VOTEU.ANY UP0, P0 (CuAssembler's sm_86 records) and NAK encodes it with a uniform destination at 81..84. Yoshi and the Mysterious Book's cbd80cb96ad38a61 votes P0 into
    UP0, guards a handle's bounds mask on !UP0 and its TEX on !P0: written to P0, UP0 stayed undefined and every lane's guard became the subgroup's"""
    VOTEU = (0x000fe20000000100, 0x00000000003f7886)   # VOTEU.ANY UP0, P0 (the game's, destination register URZ)
    VOTE = (0x000fe20000000100, 0x0000000000ff7806)    # VOTE.ANY P0, P0 (destination register RZ)
    FSETP = (0x000fca0003f1d000, 0x800000141300720b)   # FSETP.NEU.FTZ.AND P0, PT, R19, -R20, PT: P0 from R19 and R20, differs across a subgroup
    MOV_P0 = (0x000fe20000000f00, 0x0000001300060202)  # @P0 MOV R6, R19: a lane guard
    UR4_0 = (0x000fe20000000000, 0x0000003f00047c82)   # UMOV UR4, URZ
    UR4_UP0 = (0x000fe20000000000, 0x0000123400040882)   # @UP0 UMOV UR4, 0x1234: a uniform guard
    MOV_UR4 = (0x000fe20000000f00, 0x0000000400067c02)   # MOV R6, UR4

    def test_voteu_writes_the_uniform_predicate(self):
        msl = self.kept(self.FSETP, self.VOTEU, self.UR4_0, self.UR4_UP0, self.MOV_UR4, srcs=(19, 20))
        self.assertIn('simd_any', msl)
        self.assertNotIn('if (false)', msl)   # an unwritten predicate reads false, so the uniform guard never held

    def test_voteu_leaves_the_lane_predicate(self):   # nothing reads UP0: the guards (the store's !P0 too) read the comparison, not the vote
        self.assertNotIn('simd_any', self.kept(self.FSETP, self.VOTEU, self.MOV_P0, srcs=(6, 19, 20)))

    def test_vote_writes_the_lane_predicate(self):
        self.assertIn('simd_any', self.kept(self.FSETP, self.VOTE, self.MOV_P0, srcs=(6, 19, 20)))


if __name__ == '__main__':
    unittest.main()
