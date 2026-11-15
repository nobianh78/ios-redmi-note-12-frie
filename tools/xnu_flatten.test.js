// Self-check for the sysreg classifier: node tools/xnu_flatten.test.js
import assert from 'assert';
import { isImpDefSysreg, patchWord } from './xnu_flatten.js';

// Encodings taken from the iOS 16 T8015 kernel (llvm-mc) and the ARM ARM.
assert.ok(isImpDefSysreg(0xd51cf2e1));                    // msr S3_4_C15_C2_7, x1
assert.strictEqual(patchWord(0xd51cf2e1), 0xd503201f);    // -> nop
assert.ok(isImpDefSysreg(0xd538f42c));                    // mrs x12, S3_0_C15_C4_1
assert.strictEqual(patchWord(0xd538f42c), 0xd280000c);    // -> movz x12, #0
assert.ok(!isImpDefSysreg(0xd518a200));                   // msr MAIR_EL1, x0  (CRn=10)
assert.ok(!isImpDefSysreg(0xd538000c));                   // mrs x12, MIDR_EL1 (CRn=0)
assert.ok(!isImpDefSysreg(0xd503201f));                   // nop
assert.ok(!isImpDefSysreg(0xd5087620));                   // dc ivac (op0=1, SYS class)
console.log('xnu_flatten classifier OK');
