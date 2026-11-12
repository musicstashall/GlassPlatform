// Production HTML, deterministic geometry/frame queue; no duplicate UI implementation.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
let receive,reduced=false;const frames=[],messages=[],errors=[],shutdownTimers=[];
const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8'),{
 url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:reduced,addEventListener(){}});w.CSS={supports:()=>true};
 w.requestAnimationFrame=fn=>(frames.push(fn),frames.length);
 const nativeTimeout=w.setTimeout.bind(w);w.setTimeout=(fn,ms)=>ms===220?(shutdownTimers.push(fn),-shutdownTimers.length):nativeTimeout(fn,ms);
 w.chrome={webview:{addEventListener(t,fn){receive=fn;},postMessage(m){messages.push(m);}}};}
});
const d=dom.window.document;
const rows=['x64','x86','settings64'].map(id=>({id,title:id,installed:true,checked:false,operation:'update',removeSupported:true,applySupported:true,installedPackageVersion:'1.0.0',targetVersion:'2.0.0',remoteVersion:'2.0.0'}));
function state(items=rows){receive({data:{type:'state',page:'components',appMode:'setup',locale:'ru',components:items}});}
function flush(){while(frames.length)frames.shift()();}
const input=id=>d.getElementById('product-'+id);
const card=id=>input(id).closest('.c-card');
state();assert.equal(d.querySelectorAll('.c-landing,.c-removal-slot').length,0);
const list=d.querySelector('.components-installed-list');list.getBoundingClientRect=()=>({top:0,bottom:100});
// A visible existing update must not animate/reveal/reappend the shelf card.
list.scrollTop=55;const availableList=d.querySelector('[data-component-drop="available"] .c-lane-body');availableList.scrollTop=23;
card('settings64').getBoundingClientRect=()=>({top:20,bottom:90});
const installedGrid=d.getElementById('components-installed-grid'),append=installedGrid.append.bind(installedGrid);let reappends=0;
installedGrid.append=(...nodes)=>{reappends++;return append(...nodes);};
const originalOrder=[...installedGrid.querySelectorAll('.c-card')].map(x=>x.dataset.product);
card('settings64').querySelector('.comp-decision').click();flush();
assert.equal(list.scrollTop,55,'visible update does not scroll to the last card');
assert.equal(reappends,0,'unchanged shelf membership does not disturb browser scroll anchoring');
assert.deepEqual([...installedGrid.querySelectorAll('.c-card')].map(x=>x.dataset.product),originalOrder);
assert.equal(d.querySelectorAll('.c-landing').length,0);
state(rows.map(r=>({...r,checked:r.id==='settings64'})));flush();
assert.equal(list.scrollTop,55,'installed shelf scroll survives native echo');assert.equal(availableList.scrollTop,23);
installedGrid.append=append;state();list.scrollTop=0;messages.length=0;
for(const id of ['x64','x86','settings64'])card(id).getBoundingClientRect=()=>({top:120,bottom:200});
d.getElementById('components-select-all').click();flush();
assert.equal(list.scrollTop,100);
assert.equal(d.querySelectorAll('#components-installed-grid .c-landing').length,3);
assert.equal(d.querySelectorAll('[data-catalog-alias].c-landing').length,0);
assert.equal(messages.filter(m=>/^component:.*:1$/.test(m)).length,3);
const notice=d.getElementById('components-gesture-status');
assert.ok(notice.classList.contains('neutral'));assert.match(notice.textContent,/План изменён: x64, x86, settings64/);
// Host acknowledgement can rebuild cards without losing the in-flight cascade.
state(rows.map(r=>({...r,checked:true})));flush();
assert.equal(d.querySelectorAll('#components-installed-grid .c-landing').length,3);
Object.defineProperty(card('x64'),'offsetHeight',{value:96});
card('x64').querySelector('.comp-remove').click();
assert.equal(messages.at(-1),'component:x64:remove');
assert.equal(d.querySelectorAll('.c-removal-slot').length,1);
const ghost=d.querySelector('.c-removal-slot');assert.equal(ghost.getAttribute('aria-hidden'),'true');
assert.equal(ghost.querySelectorAll('[id]').length,0);
assert.equal(ghost.querySelectorAll('.comp-name,.comp-decision,input').length,0,'CRT must not retain the card content');
assert.ok(ghost.querySelector('.c-crt-screen'));assert.equal(ghost.style.height,'96px');
assert.equal(ghost.nextElementSibling?.dataset.product,'x86');
state(rows.map(r=>({...r,checked:true,operation:r.id==='x64'?'remove':'update'})));
assert.ok(d.querySelector('#components-installed-grid .c-removal-slot'),'host echo must preserve the reserved slot');
assert.ok(notice.classList.contains('neutral'));assert.match(notice.textContent,/x64 — удаление добавлено в план/);
assert.equal(card('x64').closest('#components-installed-grid'),null);
assert.match(d.getElementById('components-plan-items').textContent,/Удалить/);
assert.ok(ghost.isConnected,'slot must stay until shutdown completes');shutdownTimers.shift()();assert.equal(ghost.isConnected,false);
// Reduced motion keeps intent/scroll feedback but creates no decorative overlay.
reduced=true;state(rows);card('x86').querySelector('.comp-remove').click();
assert.equal(d.querySelectorAll('.c-removal-slot').length,0);
assert.equal(d.querySelectorAll('.c-landing').length,0);
// Actual pointer gestures: fresh OpenGlass must take Apply, not Remove.
for(const locale of ['ru','en']){
 reduced=false;
 const og={id:'openglass',title:'OpenGlass',installed:false,checked:false,operation:'install',applySupported:true,removeSupported:false,targetPayloadVersion:'3.0.2.3749',remotePayloadVersion:'3.0.2.3749'};
 receive({data:{type:'state',page:'components',appMode:'setup',locale,components:[og]}});
 const target=d.querySelector('[data-component-drop="installed"]');d.elementFromPoint=()=>target;
 const event=(type,x)=>{const e=new dom.window.MouseEvent(type,{bubbles:true,clientX:x,clientY:50,button:0});Object.defineProperties(e,{pointerType:{value:'mouse'},pointerId:{value:1}});return e;};
 card('openglass').dispatchEvent(event('pointerdown',10));d.dispatchEvent(event('pointermove',40));d.dispatchEvent(event('pointerup',40));
 assert.equal(messages.at(-1),'component:openglass:1');
 assert.ok(input('openglass').checked);
 const note=d.getElementById('components-gesture-status');assert.ok(note.classList.contains('neutral'));
 assert.match(note.textContent,locale==='ru'?/установка добавлена/:/installation planned/);
 // Cancelling a pending installation produces a neutral acknowledgement too.
 card('openglass').querySelector('.comp-decision').click();
 assert.equal(messages.at(-1),'component:openglass:0');assert.ok(note.classList.contains('neutral'));
 assert.match(note.textContent,locale==='ru'?/действие отменено/:/action cancelled/);
 // A blocked Apply is described as Apply, never as unsupported removal.
 receive({data:{type:'state',components:[{...og,applySupported:false}]}});
 const text=dom.window.componentDropText({id:'openglass',input:input('openglass'),source:'available'},'installed');
 assert.match(text,locale==='ru'?/Установка или обновление/:/Install or update/);
 assert.doesNotMatch(text,/удалени|[Rr]emoval/);
}
// Mirrored gestures: Apply (including same-version reinstall) -> undo Apply,
// never Remove. Explicit trash remains an independent action.
for(const id of ['x64','x86','settings64','settings32','openglass'])for(const newer of [false,true]){
 const row={id,title:id,installed:true,checked:false,applySupported:true,removeSupported:true,
  installedPackageVersion:'1.0.0',installedPayloadVersion:'1.0.0',
  targetVersion:newer?'2.0.0':'1.0.0',targetPayloadVersion:newer?'2.0.0':'1.0.0',operation:newer?'update':'repair'};
 state([row]);
 const move=(source,target)=>{
  d.elementFromPoint=()=>d.querySelector('[data-component-drop="'+target+'"]');
  const event=(type,x)=>{const e=new dom.window.MouseEvent(type,{bubbles:true,clientX:x,clientY:50,button:0});Object.defineProperties(e,{pointerType:{value:'mouse'},pointerId:{value:2}});return e;};
  const surface=source==='available'?d.querySelector('#components-grid .c-card[data-product="'+id+'"]'):card(id);
  surface.dispatchEvent(event('pointerdown',10));d.dispatchEvent(event('pointermove',40));d.dispatchEvent(event('pointerup',40));
 };
 move('available','installed');assert.equal(messages.at(-1),'component:'+id+':1');
 state([{...row,checked:true}]); // actual native echo must not erase undo semantics
 move('installed','available');assert.equal(messages.at(-1),'component:'+id+':0');
 assert.equal(input(id).dataset.intent,'apply');assert.equal(input(id).checked,false);
 assert.equal(d.querySelectorAll('#components-plan-items li.remove').length,0);
 card(id).querySelector('.comp-remove').click();assert.equal(messages.at(-1),'component:'+id+':remove');
 state([{...row,checked:true,operation:'remove'}]);
 move('available','installed');assert.equal(messages.at(-1),'component:'+id+':0');
 assert.equal(input(id).checked,false);assert.equal(input(id).dataset.intent,'apply');
 assert.equal(d.querySelectorAll('#components-plan-items li:not(.components-plan-empty)').length,0);
 assert.ok(!card(id).classList.contains('is-activated'));
}
// Remove -> return is Keep, independently of Apply source capability.
for(const applySupported of [false,true]){
 const row={id:'openglass',title:'OpenGlass',installed:true,checked:true,operation:'remove',applySupported,removeSupported:true,installedPayloadVersion:'3.0.2.3749'};
 state([row]);const drag={id:'openglass',input:input('openglass'),source:'available'};
 assert.equal(dom.window.componentDropValue(drag,'installed'),false);
 card('openglass').querySelector('.comp-decision').click();
 assert.equal(messages.at(-1),'component:openglass:0');assert.equal(input('openglass').checked,false);
 assert.ok(card('openglass').closest('#components-installed-grid'));
 assert.equal(d.querySelectorAll('#components-plan-items li:not(.components-plan-empty)').length,0);
 assert.ok(!card('openglass').classList.contains('is-activated'));
}
// Missing discovery version is a deferred download, not a disabled/local offer.
state([{id:'openglass',title:'OpenGlass',installed:false,checked:false,applySupported:true,downloadRequired:true,operation:'install',targetVersion:'',targetPayloadVersion:''}]);
assert.ok(card('openglass').classList.contains('active'));
assert.match(card('openglass').querySelector('.comp-version-line').textContent,/Версия уточняется|Version pending/);
card('openglass').querySelector('.comp-decision').click();
assert.equal(messages.at(-1),'component:openglass:1');
assert.ok(card('openglass').classList.contains('download-planned'));
assert.ok(!card('openglass').classList.contains('download-local'));
// Cancel the last fresh product: retain the CRT slot before the empty hint,
// and cover button, plan undo and actual pointer-drop paths.
for(const route of ['button','plan','drag']){
 while(shutdownTimers.length)shutdownTimers.shift()();
 state([{id:'fresh',title:'Fresh',installed:false,checked:true,operation:'install',applySupported:true,targetVersion:'1.0.0'}]);
 Object.defineProperty(card('fresh'),'offsetHeight',{value:72});
 if(route==='button')card('fresh').querySelector('.comp-decision').click();
 if(route==='plan')d.querySelector('.plan-cancel').click();
 if(route==='drag'){
  d.elementFromPoint=()=>d.querySelector('[data-component-drop="available"]');
  const e=type=>{const event=new dom.window.MouseEvent(type,{bubbles:true,clientX:type==='pointerdown'?10:40,clientY:50,button:0});Object.defineProperties(event,{pointerType:{value:'mouse'},pointerId:{value:3}});return event;};
  card('fresh').dispatchEvent(e('pointerdown'));d.dispatchEvent(e('pointermove'));d.dispatchEvent(e('pointerup'));
 }
 assert.equal(messages.at(-1),'component:fresh:0');
 const grid=d.getElementById('components-installed-grid'),slot=grid.querySelector('.c-removal-slot');
 assert.ok(slot);assert.equal(slot.style.height,'72px');assert.equal(grid.firstElementChild,slot);
 assert.equal(dom.window.getComputedStyle(grid.querySelector('.c-drop')).display,'none');
 state([{id:'fresh',title:'Fresh',installed:false,checked:false,operation:'install',applySupported:true,targetVersion:'1.0.0'}]);
 assert.equal(grid.firstElementChild,slot);shutdownTimers.shift()();
 assert.equal(grid.querySelector('.c-removal-slot'),null);assert.ok(!grid.classList.contains('has-removal-slot'));
}
for(const locale of ['ru','en']){
 receive({data:{type:'state',locale,components:[{id:'openglass',title:'OpenGlass',installed:false,checked:false,applySupported:true,remotePayloadVersion:'3.0.2.3749',targetPayloadVersion:''}]}});
 assert.match(card('openglass').querySelector('.comp-version-line').textContent,/3\.0\.2\.3749/);
}
assert.deepEqual(errors,[]);
dom.window.close();
console.log('PASS: composition motion, cascade, shelf-only reveal, host acknowledgement, pop ownership/IPC and reduced motion');
