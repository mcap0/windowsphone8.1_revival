// The checks of share/browser/wasm.html for engine/tools/jsclinux (made from the page: keep them the same).
var window = this;
var T = { now: function () { return preciseTime() * 1000; }, $: function () { return {}; },
  send: function (page, lines) { print(lines.join("\n")); if (/ERRORE/.test(lines.join("\n"))) throw new Error("failures"); } };

(function () {
  var lines = [], out = T.$("out"), failures = 0;
  function say(s) { lines.push(s); out.textContent = lines.join("\n"); }
  function check(ok, what) { if (!ok) failures++; say((ok ? "OK " : "ERRORE ") + what); }
  function finish() { say(failures ? failures + " errori" : "tutto OK"); T.send("wasm", lines); }

  if (typeof WebAssembly !== "object") { say("ERRORE WebAssembly assente"); T.send("wasm", lines); return; }

  // ---- A tiny assembler for the binary format (ES5) ----
  function leb(n) { var b = []; do { var x = n & 0x7f; n >>>= 7; if (n) x |= 0x80; b.push(x); } while (n); return b; }
  function sleb(n) {
    var b = [], more = true;
    while (more) { var x = n & 0x7f; n >>= 7; if ((n === 0 && !(x & 0x40)) || (n === -1 && (x & 0x40))) more = false; else x |= 0x80; b.push(x); }
    return b;
  }
  function name(s) { var b = []; for (var i = 0; i < s.length; i++) b.push(s.charCodeAt(i)); return leb(b.length).concat(b); }
  function vec(items) { var b = leb(items.length); for (var i = 0; i < items.length; i++) b = b.concat(items[i]); return b; }
  function section(id, body) { return [id].concat(leb(body.length), body); }
  function f64(v) { var a = new Uint8Array(new Float64Array([v]).buffer), b = []; for (var i = 0; i < 8; i++) b.push(a[i]); return b; }
  function body(locals, code) { var b = vec(locals).concat(code, [0x0b]); return leb(b.length).concat(b); }
  var I32 = 0x7f, I64 = 0x7e, F64 = 0x7c;
  function type(params, results) { return [0x60].concat(vec(params.map(function (p) { return [p]; })), vec(results.map(function (r) { return [r]; }))); }
  function exportFunc(n, index) { return name(n).concat([0x00], leb(index)); }

  // Types: 0 (i32,i32)->i32, 1 (i64)->i64, 2 (i32)->i64, 3 (i32,i64)->(), 4 (f64)->f64, 5 (i32)->i32, 6 ()->i32, 7 ()->()
  var types = [type([I32, I32], [I32]), type([I64], [I64]), type([I32], [I64]), type([I32, I64], []), type([F64], [F64]),
    type([I32], [I32]), type([], [I32]), type([], [])];
  // Function 0 is the import env.f (i32)->i32; ours start at 1.
  var funcs = [
    { type: 0, code: [0x20, 0, 0x20, 1, 0x6a] },                                                   // 1 add
    { type: 1, code: [0x20, 0, 0x42, 1, 0x57, 0x04, I64, 0x42, 1, 0x05, 0x20, 0, 0x20, 0, 0x42, 1, 0x7d, 0x10, 2, 0x7e, 0x0b] }, // 2 fact
    { type: 2, code: [0x20, 0, 0x29, 0, 0] },                                                      // 3 load64 (align 1)
    { type: 3, code: [0x20, 0, 0x20, 1, 0x37, 0, 0] },                                             // 4 store64 (align 1)
    { type: 4, code: [0x20, 0, 0x9f, 0x44].concat(f64(1), [0xa0]) },                               // 5 sqrt(x)+1
    { type: 5, code: [0x20, 0, 0x10, 0, 0x41, 1, 0x6a] },                                          // 6 env.f(x)+1
    { type: 6, code: [0x41, 1, 0x40, 0] },                                                         // 7 memory.grow 1
    { type: 0, code: [0x20, 0, 0x20, 1, 0x11, 5, 0] },                                             // 8 table[i](x)
    { type: 7, code: [0x00] },                                                                     // 9 unreachable
    { type: 5, locals: [[2, I32]], code: [0x02, 0x40, 0x03, 0x40, 0x20, 2, 0x20, 0, 0x4e, 0x0d, 1, 0x20, 1, 0x20, 2, 0x6a, 0x21, 1,
      0x20, 2, 0x41, 1, 0x6a, 0x21, 2, 0x0c, 0, 0x0b, 0x0b, 0x20, 1] },                            // 10 sum 0..n-1
    { type: 0, code: [0x20, 0, 0x20, 1, 0x6d] },                                                   // 11 i32.div_s
    { type: 5, code: [0x20, 0, 0x28, 2, 0] },                                                      // 12 i32.load
    { type: 5, code: [0x20, 0, 0x41, 2, 0x48, 0x04, I32, 0x20, 0, 0x05, 0x20, 0, 0x41, 1, 0x6b, 0x10, 13,
      0x20, 0, 0x41, 2, 0x6b, 0x10, 13, 0x6a, 0x0b] }                                              // 13 fib
  ];
  var names = ["add", "fact", "load64", "store64", "sqrt1", "callImport", "grow", "indirect", "trap", "sum", "divs", "load32", "fib"];
  var exportsList = names.map(function (n, i) { return exportFunc(n, i + 1); });
  exportsList.push(name("mem").concat([0x02, 0]));
  var bytes;
  // Sections in the order the format requires: types, imports, functions, table, memory, exports, elements, code.
  (function reorder() {
    var b = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
    b = b.concat(section(1, vec(types)), section(2, vec([name("env").concat(name("f"), [0x00], leb(5))])),
      section(3, vec(funcs.map(function (f) { return leb(f.type); }))), section(4, vec([[0x70, 0x00, 4]])), section(5, vec([[0x01, 1, 10]])),
      section(7, vec(exportsList)), section(9, vec([[0x00, 0x41, 0, 0x0b].concat(vec([leb(6), leb(13), leb(1)]))])),
      section(10, vec(funcs.map(function (f) { return body(f.locals || [], f.code); }))));
    bytes = new Uint8Array(b);
  })();

  function traps(f, what) {
    try { f(); check(false, what + ": nessuna eccezione"); }
    catch (e) { check(e instanceof WebAssembly.RuntimeError, what + ": " + e); }
  }
  function jsFib(n) { return n < 2 ? n : jsFib(n - 1) + jsFib(n - 2); }

  check(WebAssembly.validate(bytes), "il modulo di prova è valido (" + bytes.length + " byte)");
  var t0 = T.now();
  var instance;
  try {
    instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), { env: { f: function (x) { return x * 2; } } });
  } catch (e) { check(false, "istanza: " + e); finish(); return; }
  say("compilazione e istanza: " + Math.round(T.now() - t0) + " ms");
  var x = instance.exports;
  check(x.add(2, 3) === 5, "i32.add 2+3 = " + x.add(2, 3));
  check(x.add(0x7fffffff, 1) === -0x80000000, "i32.add avvolge: " + x.add(0x7fffffff, 1));
  try { check(String(x.fact(BigInt(20))) === "2432902008176640000", "i64 ricorsivo: 20! = " + x.fact(BigInt(20))); }
  catch (e) { check(false, "i64 con BigInt: " + e); }
  try {
    x.store64(1001, BigInt("0x0102030405060708"));
    var u8 = new Uint8Array(x.mem.buffer);
    check(u8[1001] === 8 && u8[1008] === 1, "i64.store a un indirizzo dispari: byte " + u8[1001] + ".." + u8[1008]);
    check(x.load64(1001) === BigInt("0x0102030405060708"), "i64.load a un indirizzo dispari");
    x.store64(1003, BigInt(-2));
    check(x.load64(1003) === BigInt(-2), "i64 negativo, disallineato");
  } catch (e) { check(false, "i64 in memoria: " + e); }
  check(x.sqrt1(16) === 5, "f64.sqrt(16)+1 = " + x.sqrt1(16));
  check(x.callImport(41) === 83, "chiamata a JavaScript: f(41)+1 = " + x.callImport(41));
  check(x.grow() === 1 && x.mem.buffer.byteLength === 2 * 65536, "memory.grow: " + x.mem.buffer.byteLength + " byte");
  check(x.indirect(10, 1) === 55, "call_indirect fib(10) = " + x.indirect(10, 1));
  check(x.indirect(5, 0) === 11, "call_indirect verso l'import = " + x.indirect(5, 0));
  traps(function () { x.indirect(1, 2); }, "call_indirect con firma sbagliata");
  traps(function () { x.indirect(1, 3); }, "call_indirect a un posto vuoto");
  traps(function () { x.indirect(1, 9); }, "call_indirect fuori dalla tabella");
  traps(function () { x.trap(); }, "unreachable");
  traps(function () { x.divs(7, 0); }, "divisione per zero");
  traps(function () { x.divs(-0x80000000, -1); }, "overflow di i32.div_s");
  check(x.load32(131068) === 0, "i32.load all'ultimo byte valido");
  traps(function () { x.load32(131070); }, "i32.load oltre la fine");
  traps(function () { x.load32(-1); }, "i32.load a 0xffffffff");
  check(x.sum(1000) === 499500, "ciclo: somma 0..999 = " + x.sum(1000));

  // Speed: the same recursion in Wasm and in JavaScript (both warmed first).
  x.fib(15); jsFib(15);
  var a = T.now(), wv = x.fib(25), wt = T.now() - a;
  a = T.now(); var jv = jsFib(25), jt = T.now() - a;
  check(wv === jv, "fib(25) = " + wv + ": Wasm " + Math.round(wt) + " ms, JavaScript " + Math.round(jt) + " ms");
  a = T.now(); var s = x.sum(10000000), st = T.now() - a;
  say("ciclo di 10 milioni in Wasm: " + Math.round(st) + " ms");

  // A module of many functions: what the compilation costs (each function is compiled at its first call).
  var many = [];
  for (var i = 0; i < 1500; i++) many.push({ type: 0, code: [0x20, 0, 0x20, 1, 0x6a, 0x41].concat(sleb(i), [0x6a]) });
  var big = (function () {
    var b = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];
    b = b.concat(section(1, vec([type([I32, I32], [I32])])), section(3, vec(many.map(function () { return [0]; }))),
      section(7, vec([exportFunc("last", many.length - 1)])), section(10, vec(many.map(function (f) { return body([], f.code); }))));
    return new Uint8Array(b);
  })();
  a = T.now();
  try {
    var bigInstance = new WebAssembly.Instance(new WebAssembly.Module(big), {});
    var mid = T.now();
    check(bigInstance.exports.last(1, 2) === 1502, "modulo di 1500 funzioni (" + big.length + " byte): istanza " + Math.round(mid - a) + " ms, prima chiamata " + Math.round(T.now() - mid) + " ms");
  } catch (e) { check(false, "modulo grande: " + e); }

  // SIMD is not done on this CPU: a module with it must be refused, not crash.
  var simd = new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 1, 5, 1, 0x60, 0, 1, 0x7b, 3, 2, 1, 0, 10, 22, 1, 20, 0, 0xfd, 0x0c].concat(
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], [0x0b]));
  say("SIMD: " + (WebAssembly.validate(simd) ? "accettato" : "non disponibile (atteso su ARMv7)"));

  // Streaming compilation from a Response, and the asynchronous API.
  var pending = 2;
  function done() { if (--pending === 0) finish(); }
  WebAssembly.instantiate(bytes, { env: { f: function (v) { return v + 100; } } }).then(function (r) {
    check(r.instance.exports.callImport(1) === 102, "WebAssembly.instantiate (asincrono)");
  }, function (e) { check(false, "WebAssembly.instantiate: " + e); }).then(done);
  if (WebAssembly.instantiateStreaming && window.Response) {
    WebAssembly.instantiateStreaming(new Response(bytes, { headers: { "Content-Type": "application/wasm" } }), { env: { f: function (v) { return v; } } }).then(function (r) {
      check(r.instance.exports.add(20, 22) === 42, "instantiateStreaming da una Response");
    }, function (e) { check(false, "instantiateStreaming: " + e); }).then(done);
  } else { say("instantiateStreaming: non provato qui (nessuna Response)"); done(); }
})();
