// (func (param externref externref i32) (result externref) local.get 0 local.get 1 local.get 2 select (result externref))
function mod(code) {
  var b = [0,0x61,0x73,0x6d,1,0,0,0, 1,8,1,0x60,3,0x6f,0x6f,0x7f,1,0x6f, 3,2,1,0, 7,5,1,1,0x66,0,0];
  var body = [0].concat(code, [0x0b]);
  b = b.concat([10, body.length + 2, 1, body.length], body);
  return new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(b))).exports.f;
}
var typed = mod([0x20,0,0x20,1,0x20,2,0x1c,1,0x6f]);
var a = {n:1}, c = {n:2};
function show(v) { return v === a ? "a" : v === c ? "c" : typeof v + ":" + String(v); }
print("typed 1:", show(typed(a, c, 1)), " typed 0:", show(typed(a, c, 0)));
print("ints:", show(typed(1, 2, 1)), show(typed(1, 2, 0)), "null:", show(typed(null, c, 1)), show(typed(a, null, 0)));
print("str:", show(typed("x", "y", 0)), "dbl:", show(typed(1.5, 2.5, 0)), show(typed(1.5, 2.5, 1)));
