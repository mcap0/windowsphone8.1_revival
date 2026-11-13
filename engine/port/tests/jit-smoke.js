// A first check of the 32-bit JIT on engine/tools/jsclinux: hot code that reaches Baseline and DFG must keep giving
// the interpreter's answers. Prints one line per case and the total; exit status 3 on a wrong answer.
"use strict";
let failures = 0;
function check(ok, what) { if (!ok) failures++; print((ok ? "OK " : "ERRORE ") + what); }

function sum(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return s; }
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
function doubles(n) { let x = 0.5; for (let i = 0; i < n; i++) x = x * 1.0000001 + Math.sqrt(i); return x; }
function objects(n) {
    let total = 0;
    for (let i = 0; i < n; i++) {
        const o = i & 1 ? { a: i, b: 2 } : { b: 3, a: i };   // two shapes: polymorphic access
        total += o.a + o.b;
    }
    return total;
}
function strings(n) { let s = ""; for (let i = 0; i < n; i++) s += String.fromCharCode(65 + i % 26); return s.length + s.charCodeAt(n - 1); }
function arrays(n) { const a = []; for (let i = 0; i < n; i++) a.push(i * 2); return a.reduce((x, y) => x + y, 0); }
function int32Overflow(n) { let x = 0x7ffffff0 | 0; for (let i = 0; i < n; i++) x = (x + 7) | 0; return x; }
function exceptions(n) { let c = 0; for (let i = 0; i < n; i++) { try { if (i % 3 == 0) throw new Error("x"); c++; } catch (e) { c += 2; } } return c; }
function closures(n) { const fs = []; for (let i = 0; i < 10; i++) fs.push(x => x + i); let t = 0; for (let i = 0; i < n; i++) t += fs[i % 10](i); return t; }
function bigint(n) { let x = 1n; for (let i = 0; i < n; i++) x = (x * 3n) % 1000000007n; return x; }

const start = Date.now();
for (let round = 0; round < 3; round++) {
    check(sum(1e6) === 499999500000, "sum " + round);
    check(fib(22) === 17711, "fib " + round);
    check(Math.abs(doubles(1e5) - doubles(1e5)) === 0 && doubles(10) > 0, "doubles " + round);
    check(objects(1e5) === 5000000000 - 50000 + 250000 - 50000 + 50000, "objects " + round + " " + objects(1e5));
    check(strings(5000) === 5000 + (65 + 4999 % 26), "strings " + round);
    check(arrays(1e5) === 9999900000, "arrays " + round);
    check(int32Overflow(100) === ((0x7ffffff0 + 700) | 0), "int32 " + round);
    check(exceptions(3000) === 4000, "exceptions " + round);
    check(closures(1e5) === 4999950000 + 450000, "closures " + round);
    check(bigint(2000) === bigint(2000) && typeof bigint(1) === "bigint", "bigint " + round);
}
print((failures ? failures + " errori" : "tutto OK") + " in " + (Date.now() - start) + " ms");
if (failures) throw new Error(failures + " failures");
