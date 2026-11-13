function leb(n){var b=[];do{var x=n&0x7f;n>>>=7;if(n)x|=0x80;b.push(x);}while(n);return b;}
function vec(a){var b=leb(a.length);a.forEach(function(x){b=b.concat(x);});return b;}
function sec(id,body){return [id].concat(leb(body.length),body);}
var I64=0x7e;
function mod(codes){ // each: (i64,i64)->i64
  var b=[0,0x61,0x73,0x6d,1,0,0,0];
  b=b.concat(sec(1,vec([[0x60,2,I64,I64,1,I64]])),sec(3,vec(codes.map(function(){return [0];}))),
    sec(7,vec(codes.map(function(_,i){return [2,0x66,0x30+i,0,i];}))),
    sec(10,vec(codes.map(function(c){var body=[0].concat(c,[0x0b]);return leb(body.length).concat(body);}))));
  return new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(b))).exports;
}
var ops={div_s:0x7f,div_u:0x80,rem_s:0x81,rem_u:0x82};
var ref={div_s:function(a,b){return BigInt.asIntN(64,a/b);},div_u:function(a,b){return BigInt.asIntN(64,BigInt.asUintN(64,a)/BigInt.asUintN(64,b));},
  rem_s:function(a,b){return a%b;},rem_u:function(a,b){return BigInt.asIntN(64,BigInt.asUintN(64,a)%BigInt.asUintN(64,b));}};
var vals=[1n,2n,7n,-1n,-7n,0x100000000n,0x7fffffffffffffffn,-0x8000000000000000n,0xffffffffn,123456789012345n,-0x100000000n,3n];
var bad=0,n=0;
for (var name in ops) {
  var op=ops[name];
  var x=mod([
    [0x20,0,0x20,1,op],                 // a op b
    [0x20,1,0x20,0,op],                 // b op a (swapped registers)
    [0x20,0,0x42,7,op],                 // a op 7
    [0x42,0xe4,0x00,0x20,1,op],         // 100 op b
    [0x20,0,0x20,0,0x20,1,op,op],       // a op (a op b)   (may divide by zero: skip then)
    [0x20,0,0x20,1,0x7c,0x20,1,op]      // (a+b) op b
  ]);
  vals.forEach(function(a){vals.forEach(function(b){
    function chk(what,got,want){n++;var g;try{g=got();}catch(e){g="trap:"+e.message;}var w;try{w=want();}catch(e){w="x";}
      if(w==="x")return; if(g!==w){bad++;if(bad<25)print("BAD",name,what,a,b,"got",g,"want",w);}}
    var ovf = name==="div_s" && a===-0x8000000000000000n && b===-1n;
    if(!ovf){
    chk("a.b",function(){return x.f0(a,b);},function(){return ref[name](a,b);});
    chk("b.a",function(){return x.f1(b,a);},function(){return ref[name](a,b);});
    chk("a.7",function(){return x.f2(a,b);},function(){return ref[name](a,7n);});
    chk("100.b",function(){return x.f3(a,b);},function(){return ref[name](100n,b);});
    chk("a+b.b",function(){return x.f5(a,b);},function(){return ref[name](BigInt.asIntN(64,a+b),b);});
    }
  });});
  try{x.f0(5n,0n);bad++;print("BAD no trap",name);}catch(e){if(!/Division by zero/.test(e.message)){bad++;print("BAD trap",name,e.message);}}
  try{x.f3(5n,0n);bad++;print("BAD no trap c",name);}catch(e){if(!/Division by zero/.test(e.message)){bad++;print("BAD trap c",name,e.message);}}
}
print(n,"checks,",bad,"bad");
