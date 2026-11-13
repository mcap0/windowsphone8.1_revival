function leb(n){var b=[];do{var x=n&0x7f;n>>>=7;if(n)x|=0x80;b.push(x);}while(n);return b;}
function name(s){var b=[];for(var i=0;i<s.length;i++)b.push(s.charCodeAt(i));return leb(b.length).concat(b);}
function vec(a){var b=leb(a.length);a.forEach(function(x){b=b.concat(x);});return b;}
function sec(id,body){return [id].concat(leb(body.length),body);}
var X=0x6f,I=0x7f,RX=[0x64,0x6f];
function ty(p,r){return [0x60].concat(vec(p.map(function(x){return [].concat(x);})),vec(r.map(function(x){return [].concat(x);})));}
// one module per builtin: import it and re-export through a wrapper that forwards its params
function wrap(bname,p,r){
  var b=[0,0x61,0x73,0x6d,1,0,0,0];
  var code=[];p.forEach(function(_,i){code.push(0x20,i);});code.push(0x10,0,0x0b);
  var body=[0].concat(code);
  b=b.concat(sec(1,vec([ty(p,r)])),sec(2,vec([name("wasm:js-string").concat(name(bname),[0,0])])),sec(3,vec([[0]])),
    sec(7,vec([name("f").concat([0,1])])),sec(10,vec([leb(body.length).concat(body)])));
  return new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(b),{builtins:["js-string"]}),{}).exports.f;
}

function t(what,f){try{print(what,"=",JSON.stringify(f()));}catch(e){print(what,"THROWS",e);}}
t("length",function(){return wrap("length",[X],[I])("hello");});
t("concat",function(){return wrap("concat",[X,X],[RX])("ab","cd");});
t("substring",function(){return wrap("substring",[X,I,I],[RX])("abcdefgh",2,5);});
t("charCodeAt",function(){return wrap("charCodeAt",[X,I],[I])("abc",1);});
t("codePointAt",function(){return wrap("codePointAt",[X,I],[I])("abc",2);});
t("fromCharCode",function(){return wrap("fromCharCode",[I],[RX])(65);});
t("fromCodePoint",function(){return wrap("fromCodePoint",[I],[RX])(0x1F600).length;});
t("equals",function(){return [wrap("equals",[X,X],[I])("a","a"),wrap("equals",[X,X],[I])("a","b"),wrap("equals",[X,X],[I])(null,null)];});
t("compare",function(){var c=wrap("compare",[X,X],[I]);return [c("a","b"),c("b","a"),c("a","a")];});
t("test",function(){var c=wrap("test",[X],[I]);return [c("a"),c(1),c(null)];});
t("cast",function(){return wrap("cast",[X],[RX])("zz");});
t("cast bad",function(){return wrap("cast",[X],[RX])(5);});
t("charCodeAt oob",function(){return wrap("charCodeAt",[X,I],[I])("abc",9);});
