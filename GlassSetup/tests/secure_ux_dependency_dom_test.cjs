// Production HTML, deterministic time, real click and pointer drag handlers.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
let receive,now=0,id=0;const timers=new Map(),messages=[],errors=[];
const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e.message));
const dom=new JSDOM(html,{url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.Date.now=()=>now;w.setTimeout=(fn,ms=0)=>{timers.set(++id,{fn,at:now+ms});return id;};w.clearTimeout=x=>timers.delete(x);
 w.chrome={webview:{addEventListener(t,fn){if(t==='message')receive=fn;},postMessage:m=>messages.push(m)}};}});
const w=dom.window,d=w.document;
function advance(ms){const end=now+ms;let limit=2000;for(;;){const entry=[...timers].sort((a,b)=>a[1].at-b[1].at)[0];if(!entry||entry[1].at>end)break;assert.ok(--limit);now=entry[1].at;timers.delete(entry[0]);entry[1].fn();}now=end;}
const settings={id:'settings64',title:'GlassSettings x64',installed:true,installedVersion:'1.0.0',targetVersion:'2.0.0',operation:'update',applySupported:true,removeSupported:true,checked:false};
const sut={id:'secureUxTheme',title:'SecureUxTheme',installed:true,installedVersion:'4.0.0',operation:'repair',applySupported:false,removeSupported:true,checked:false,removalDependencyRequired:true,removalDependencyId:'settings64'};
function send(rows,lang='ru'){receive({data:{type:'state',appMode:'setup',page:'components',locale:lang,components:rows,componentsLoading:false}});}
function input(id){return d.querySelector('#product-'+id);}
function card(id){return input(id).closest('.c-card');}
function note(){return d.querySelector('#components-gesture-status');}
function activity(type='click'){d.dispatchEvent(new w.MouseEvent(type,{bubbles:true,detail:1}));}
function dragSut(){const target=d.querySelector('[data-component-drop="available"]');d.elementFromPoint=()=>target;
 const event=(type,x)=>{const e=new w.MouseEvent(type,{bubbles:true,clientX:x,clientY:50,button:0});Object.defineProperties(e,{pointerType:{value:'mouse'},pointerId:{value:1}});return e;};
 card('secureUxTheme').dispatchEvent(event('pointerdown',10));d.dispatchEvent(event('pointermove',40));d.dispatchEvent(event('pointerup',40));}
for(const lang of ['ru','en']){
 send([settings,sut],lang);let before=messages.length;card('secureUxTheme').querySelector('.comp-remove').click();
 assert.equal(messages.length,before);assert.equal(input('secureUxTheme').checked,false);assert.ok(d.querySelector('#components-installed-grid').contains(card('secureUxTheme')));assert.ok(note().classList.contains('warn'));
 assert.match(note().textContent,/GlassSettings x64/);advance(5000);assert.ok(note().classList.contains('show'),'no activity: no timer expiry');for(const type of ['pointermove','pointerdown','keydown'])activity(type);advance(5000);assert.ok(note().classList.contains('show'),'movement, press and keyboard are not clicks');d.dispatchEvent(new w.MouseEvent('click',{bubbles:true,detail:0}));advance(0);assert.ok(note().classList.contains('show'),'keyboard/programmatic click is ignored');receive({data:{type:'component-notice-click'}});advance(0);assert.ok(!note().classList.contains('show'),'native footer click dismisses HTML notice');
 // The browser's trailing click after pointerup is part of the same drag,
 // not the NEXT user click; it must not arm an automatic one-second expiry.
 dragSut();card('secureUxTheme').dispatchEvent(new w.MouseEvent('click',{bubbles:true,detail:1}));advance(5000);assert.ok(note().classList.contains('show'));
 activity('auxclick');advance(0);assert.ok(!note().classList.contains('show'));
 // Real drag repeats the refusal and starts a fresh one-second minimum.
 dragSut();assert.equal(messages.length,before);assert.ok(note().classList.contains('warn'));activity('auxclick');advance(999);assert.ok(note().classList.contains('show'));advance(1);assert.ok(!note().classList.contains('show'));
 // Wheel/trackpad has detail=0, but is activity; minimum still applies.
 dragSut();d.dispatchEvent(new w.WheelEvent('wheel',{bubbles:true,deltaY:80}));advance(999);assert.ok(note().classList.contains('show'));advance(1);assert.ok(!note().classList.contains('show'));
 dragSut();advance(1500);receive({data:{type:'component-notice-scroll'}});advance(0);assert.ok(!note().classList.contains('show'),'native scroll bridge');
 // Wrong-architecture Settings does not satisfy the prerequisite.
 send([{...settings,id:'settings32',title:'GlassSettings x86',checked:true},settings,sut],lang);before=messages.length;card('secureUxTheme').querySelector('.comp-remove').click();assert.equal(messages.length,before);
 // No auto selection; explicitly update Settings, then remove SUT.
 card('settings64').querySelector('.comp-decision').click();assert.equal(messages.at(-1),'component:settings64:1');
 card('secureUxTheme').querySelector('.comp-remove').click();assert.equal(messages.at(-1),'component:secureUxTheme:remove');
 const plan=d.querySelector('#components-plan-items').textContent;assert.ok(plan.indexOf('GlassSettings x64')<plan.indexOf('SecureUxTheme'));
 // Host echo preserves order; cancelling prerequisite cancels dependent intent.
 send([{...settings,checked:true},{...sut,checked:true,operation:'remove'}],lang);
 before=messages.length;card('settings64').querySelector('.comp-decision').click();assert.deepEqual(messages.slice(before),['component:secureUxTheme:0','component:settings64:0']);assert.equal(input('secureUxTheme').checked,false);
 // No Settings at all: actual drag moves the card and pins the red risk summary.
 send([{...sut,removalDependencyRequired:false,removalWithoutSettings:true}],lang);dragSut();
 assert.equal(messages.at(-1),'component:secureUxTheme:remove');assert.ok(d.querySelector('#components-grid').contains(card('secureUxTheme')));assert.ok(note().classList.contains('danger'));assert.equal(note().getAttribute('role'),'alert');
 const notices=d.querySelector('#components-plan-notices');assert.equal(notices.hidden,false);assert.ok(notices.classList.contains('danger'));assert.doesNotMatch(notices.textContent,/ответственность|responsibility/i);
 advance(500);activity();advance(499);assert.ok(note().classList.contains('show'));advance(1);assert.ok(!note().classList.contains('show'));assert.equal(notices.hidden,false);
 notices.querySelector('button').click();assert.match(d.querySelector('#components-warning-text').textContent,/ответственность|responsibility/i);d.querySelector('#components-warning-ok').click();
 d.querySelector('.plan-cancel').click();assert.equal(notices.hidden,true);
 // Click follows the same no-Settings risk path and survives a host echo.
 send([{...sut,removalDependencyRequired:false,removalWithoutSettings:true}],lang);
 card('secureUxTheme').querySelector('.comp-remove').click();assert.ok(note().classList.contains('danger'));
 send([{...sut,checked:true,operation:'remove',removalDependencyRequired:false,removalWithoutSettings:true}],lang);
 assert.ok(d.querySelector('#components-plan-notices').classList.contains('danger'));
 d.querySelector('.plan-cancel').click();
 // Missing Settings but native installation is explicitly planned: no waiver.
 send([{...settings,installed:false,checked:true,operation:'install'},{...sut,removalDependencyRequired:false,removalWithoutSettings:true}],lang);
 card('secureUxTheme').querySelector('.comp-remove').click();assert.equal(d.querySelector('#components-plan-notices').hidden,true);
}
assert.deepEqual(errors,[]);dom.window.close();
console.log('PASS: production dependency UI: RU/EN click/drag refusal, OS architecture, explicit prerequisite/order/undo, no-Settings red risk + pinned Details, clicks + scrolling/minimum 1000ms');
