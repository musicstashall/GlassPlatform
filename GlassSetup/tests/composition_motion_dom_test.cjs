// Production HTML, deterministic geometry/frame queue; no duplicate UI implementation.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
let receive,reduced=false;const frames=[],messages=[],errors=[];
const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8'),{
 url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:reduced,addEventListener(){}});w.CSS={supports:()=>true};
 w.requestAnimationFrame=fn=>(frames.push(fn),frames.length);
 w.chrome={webview:{addEventListener(t,fn){receive=fn;},postMessage(m){messages.push(m);}}};}
});
const d=dom.window.document;
const rows=['x64','x86','settings64'].map(id=>({id,title:id,installed:true,checked:false,operation:'update',removeSupported:true,applySupported:true,installedPackageVersion:'1.0.0',targetVersion:'2.0.0',remoteVersion:'2.0.0'}));
function state(items=rows){receive({data:{type:'state',page:'components',appMode:'setup',locale:'ru',components:items}});}
function flush(){while(frames.length)frames.shift()();}
const input=id=>d.getElementById('product-'+id);
const card=id=>input(id).closest('.c-card');
state();assert.equal(d.querySelectorAll('.c-landing,.c-pop-ghost').length,0);
const list=d.querySelector('.components-installed-list');list.getBoundingClientRect=()=>({top:0,bottom:100});
for(const id of ['x64','x86','settings64'])card(id).getBoundingClientRect=()=>({top:120,bottom:200});
d.getElementById('components-select-all').click();flush();
assert.equal(list.scrollTop,100);
assert.equal(d.querySelectorAll('#components-installed-grid .c-landing').length,3);
assert.equal(d.querySelectorAll('[data-catalog-alias].c-landing').length,0);
assert.equal(messages.filter(m=>/^component:.*:1$/.test(m)).length,3);
// Host acknowledgement can rebuild cards without losing the in-flight cascade.
state(rows.map(r=>({...r,checked:true})));flush();
assert.equal(d.querySelectorAll('#components-installed-grid .c-landing').length,3);
card('x64').querySelector('.comp-remove').click();
assert.equal(messages.at(-1),'component:x64:remove');
assert.equal(d.querySelectorAll('.c-pop-ghost').length,1);
const ghost=d.querySelector('.c-pop-ghost');assert.equal(ghost.getAttribute('aria-hidden'),'true');
assert.equal(ghost.querySelectorAll('[id]').length,0);
assert.equal(card('x64').closest('#components-installed-grid'),null);
assert.match(d.getElementById('components-plan-items').textContent,/Удалить/);
// Reduced motion keeps intent/scroll feedback but creates no decorative overlay.
reduced=true;state(rows);card('x86').querySelector('.comp-remove').click();
assert.equal(d.querySelectorAll('.c-pop-ghost').length,1);
assert.equal(d.querySelectorAll('.c-landing').length,0);
assert.deepEqual(errors,[]);
dom.window.close();
console.log('PASS: composition motion, cascade, shelf-only reveal, host acknowledgement, pop ownership/IPC and reduced motion');
