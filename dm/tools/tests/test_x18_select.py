"""The stack-free CSEL rewrite must preserve the selected value, flags and guest stack."""
import ast
import re
import struct
import unittest

from dm.tools.tests import test_x18_sysreg as slots

SITE = slots.SITE
PATCH = slots.PATCH_BASE
KEY = slots.KEY_PAGE
TSD = slots.TSD
TCB = 0x50000
K18 = slots.SLOT['K18']
scope = dict(slots.scope, patch=bytearray(), fix=[])
functions = {'select_x18', 'select_scratch', 'site_select'}
constants = {'SELECT_READS', 'SELECT_WRITES'}
nodes: list[ast.stmt] = [
    n for n in slots.tree.body if isinstance(n, ast.FunctionDef) and n.name in functions
    or isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id in constants for t in n.targets)
]
# The emitter's existing helpers use their module globals, including the test patch buffer.
helpers = {'here', 'emit', 'emit_tp', 'emit_key_addr', 'emit_slot_addr'}
nodes.extend(n for n in slots.tree.body if isinstance(n, ast.FunctionDef) and n.name in helpers)
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(slots.TOOLS / 'patch_nso.py'), 'exec'), scope)


def select(rd, rn, rm, wide):
    # csel Rd,Rn,Rm,gt
    return 0x1A80C000 | (wide << 31) | (rm << 16) | (rn << 5) | rd


def emit_select(word, temp=9, target='apple'):
    ins = scope['dis'](word, SITE)
    saved = scope['TARGET']
    scope['TARGET'] = target
    try:
        scope['patch'].clear()
        scope['fix'].clear()
        scope['site_select'](ins, scope['select_x18'](ins), temp)
        return bytes(scope['patch']), list(scope['fix'])
    finally:
        scope['TARGET'] = saved


class ScratchProof(unittest.TestCase):
    def scratch(self, code):
        start = SITE + 4
        ins = {start + 4 * n: scope['dis'](word, start + 4 * n) for n, word in enumerate(code)}
        return scope['select_scratch'](SITE, ins, start + len(code) * 4, 11)

    def test_write_on_both_paths(self):
        branch = slots.nso_asm.BEQ(SITE + 4, SITE + 16)
        escape = slots.nso_asm.B(SITE + 12, SITE + 0x100)
        kill = slots.nso_asm.MOVX(9, 0)
        self.assertEqual(self.scratch([branch, kill, escape, kill]), 9)
        self.assertIsNone(self.scratch([branch, kill, escape, slots.nso_asm.ADDI(9, 9, 1)]))

    def test_load_address_is_read_before_overwrite(self):
        self.assertIsNone(self.scratch([slots.nso_asm.LDRX(9, 9)]))

    def test_call_may_read_any_unproven_register(self):
        self.assertIsNone(self.scratch([slots.nso_asm.BL(SITE + 4, SITE + 0x100), 0xD65F03C0]))


class StaticShape(unittest.TestCase):
    def test_android_uses_tpidr_el0_slot_base(self):
        for wide in (False, True):
            for rn, rm in ((11, 18), (18, 12), (18, 31)):
                with self.subTest(wide=wide, rn=rn, rm=rm):
                    code, fix = emit_select(select(11, rn, rm, wide), temp=9, target='android')
                    ins = list(slots.md.disasm(code, PATCH))
                    self.assertEqual(len(ins) * 4, len(code))
                    text = [f'{i.mnemonic} {i.op_str}' for i in ins]
                    self.assertFalse(any('tpidrro_el0' in s for s in text), text)
                    self.assertFalse([s for s in text if re.search(r'\b[xw]18\b', s)], text)
                    self.assertEqual(text[1:3], ['mrs x9, tpidr_el0', 'ldr x9, [x9, #0x10]'])
                    self.assertEqual(fix, [(3 * 4, 'K18')])
                    self.assertEqual(ins[3].mnemonic, 'adrp')
                    self.assertEqual(text[6], f'ldr {"x11" if wide else "w11"}, [x9]')
                    self.assertEqual((ins[7].mnemonic, ins[7].operands[0].imm), ('b', SITE + 4))
                    self.assertTrue(ins[0].mnemonic.startswith('b.'))
                    self.assertEqual(ins[0].operands[0].imm, ins[8].address)
                    self.assertEqual((ins[-1].mnemonic, ins[-1].operands[0].imm), ('b', SITE + 4))
                    other = rm if rn == 18 else rn
                    self.assertEqual(len(ins) - 8, 1 if wide and other == 11 else 2)

    def test_apple_keeps_tpidrro_el0_slot_base(self):
        for wide in (False, True):
            for rn, rm in ((11, 18), (18, 12)):
                with self.subTest(wide=wide, rn=rn, rm=rm):
                    code, fix = emit_select(select(11, rn, rm, wide), temp=9, target='apple')
                    ins = list(slots.md.disasm(code, PATCH))
                    self.assertEqual(len(ins) * 4, len(code))
                    text = [f'{i.mnemonic} {i.op_str}' for i in ins]
                    self.assertEqual(text[1], 'mrs x9, tpidrro_el0')
                    self.assertFalse(any('tpidr_el0' in s for s in text), text)
                    self.assertEqual(fix, [(2 * 4, 'K18')])
                    self.assertEqual(ins[0].operands[0].imm, ins[7].address)
                    self.assertEqual((ins[6].mnemonic, ins[6].operands[0].imm), ('b', SITE + 4))
                    self.assertEqual((ins[-1].mnemonic, ins[-1].operands[0].imm), ('b', SITE + 4))

    def test_no_direct_tpidrro_outside_emit_tp(self):
        outside = [
            getattr(stmt, 'name', f'line {stmt.lineno}')
            for stmt in slots.tree.body
            if not (isinstance(stmt, ast.FunctionDef) and stmt.name == 'emit_tp')
            and any(isinstance(n, ast.Name) and n.id == 'MRS_TPIDRRO' for n in ast.walk(stmt))
        ]
        self.assertEqual(outside, [])


@unittest.skipIf(slots.uc is None, 'optional ARM64 execution dependency: unicorn')
class Run(unittest.TestCase):
    def test_select_with_unmapped_stack(self):
        uc, arm = slots.uc, slots.arm
        assert uc is not None and arm is not None
        for target in ('apple', 'android'):
            k18 = 0 if target == 'android' else K18
            for wide in (False, True):
                for rn, rm in ((11, 18), (18, 12), (18, 31)):
                    for gt, nzcv in ((True, 0), (False, 1 << 30)):
                        with self.subTest(target=target, wide=wide, rn=rn, rm=rm, gt=gt):
                            code, _ = emit_select(select(11, rn, rm, wide), 9, target)
                            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
                            bases = (0, PATCH, KEY, TSD, TCB) if target == 'android' else (0, PATCH, KEY, TSD)
                            for base in bases: mu.mem_map(base, 0x10000)
                            mu.mem_write(PATCH, code)
                            mu.mem_write(KEY, struct.pack('<Q', k18))
                            value18 = 0xF00D00005678ABCD
                            mu.mem_write(TSD + k18, struct.pack('<Q', value18))
                            if target == 'android':
                                mu.mem_write(0, struct.pack('<Q', 0xBAD0BAD0BAD0BAD0))
                                mu.mem_write(TCB + 0x10, struct.pack('<Q', TSD))
                                mu.reg_write(arm.UC_ARM64_REG_TPIDR_EL0, TCB)
                                mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, 0)
                            else:
                                mu.reg_write(arm.UC_ARM64_REG_TPIDRRO_EL0, TSD)
                            mu.reg_write(arm.UC_ARM64_REG_SP, 0x10000000)
                            mu.reg_write(arm.UC_ARM64_REG_NZCV, nzcv)
                            values = {9: 9, 11: 0xBAAC000012345678, 12: 0xCDEF0000CAFE0123, 18: 0xDEAD}
                            for reg, value in values.items(): mu.reg_write(arm.UC_ARM64_REG_X0 + reg, value)
                            mu.emu_start(PATCH, SITE + 4)
                            chosen = rn if gt else rm
                            expected = value18 if chosen == 18 else values.get(chosen, 0)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X11), expected if wide else expected & 0xFFFFFFFF)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_SP), 0x10000000)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_NZCV), nzcv)
                            self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X18), values[18])
                            self.assertEqual(mu.mem_read(TSD + k18, 8), struct.pack('<Q', value18))
                            if chosen != 18: self.assertEqual(mu.reg_read(arm.UC_ARM64_REG_X9), values[9])


if __name__ == '__main__':
    unittest.main()
