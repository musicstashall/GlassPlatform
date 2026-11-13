const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
let receive;const messages=[],errors=[];const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(html,{url:'https://glassplatform.invalid/?lang=ru',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(t,cb){if(t==='message')receive=cb;},postMessage(m){messages.push(m);}}};}});
const d=dom.window.document;

const rows=['x64','x86','settings64','settings32','openglass','secureUxTheme'].map(id=>({id,title:id,installed:false,checked:false,operation:'install',applySupported:true,targetVersion:'3.0.2.3749',targetPayloadVersion:'3.0.2.3749',warning:''}));
const send=(components,extra={})=>receive({data:{type:'state',appMode:'setup',page:'components',locale:'ru',components,componentsLoading:false,...extra}});
const button=()=>d.querySelector('#components-select-all');
send(rows);
assert.equal(d.querySelectorAll('#components-installed-grid .c-card').length,0);
assert.equal(d.querySelectorAll('.c-card.has-warning').length,0);
assert.equal(button().disabled,false,'fresh install must offer recommendations');
let before=messages.length;button().click();
for(const row of rows.filter(x=>x.id!=='openglass')){assert.ok(d.querySelector('#product-'+row.id).checked,row.id);assert.ok(messages.slice(before).includes('component:'+row.id+':1'));}
assert.equal(button().disabled,true);assert.match(button().textContent,/Всё выбрано/);
send(rows.map(x=>({...x,checked:true})));send(rows);assert.equal(button().disabled,false,'native echo with no selection must not leave button stuck');
send(rows.map(x=>({...x,applySupported:false})));assert.equal(button().disabled,true);
send(rows.map(x=>({...x,disabled:true})));assert.equal(button().disabled,true);
send(rows,{componentsLoading:true});assert.equal(button().disabled,true);
// Existing healthy products are not blindly reinstalled; changed helper is recommended.
send([{...rows[0],installed:true,installedVersion:'3.0.2.3749',operation:'repair'},
 {...rows[2],installed:true,installedVersion:'3.0.2.3749',operation:'repair',nativeSourceDiffers:true}]);
assert.equal(button().disabled,false);before=messages.length;button().click();
assert.equal(d.querySelector('#product-x64').checked,false);
assert.equal(d.querySelector('#product-settings64').checked,true);
assert.deepEqual(messages.slice(before),['component:settings64:1']);
// OG and Resources are never recommendations, even when fresh, outdated or explicitly marked recommended.
for(const locale of ['ru','en'])for(const installed of [false,true]){
 const og={...rows[4],installed,installedPayloadVersion:'3.0.1.1',targetPayloadVersion:'3.0.2.3749'};
 const graphics={id:'openglassResources',title:'Resources',installed,checked:false,operation:'repair',
  targetVersion:'2.0.0.115',installedPackageVersion:'2.0.0.115',applySupported:false,pairedApplySupported:true};
 send([{...og,recommended:true},{...graphics,recommended:true}],{locale});before=messages.length;button().click();
 assert.deepEqual(messages.slice(before),[]);
 assert.equal(d.querySelector('#product-openglass').checked,false);
 assert.equal(d.querySelector('#product-openglassResources').checked,false);
 // Manual OG selection remains available; recommendation exclusion is not a ban.
 d.querySelector('#components-grid [data-product="openglass"] .comp-decision').click();
 assert.equal(messages.at(-1),'component:openglass:1');

}
// An installed retained SUT is not a seventh planned action after Accept all.
for(const locale of ['ru','en']){
 const seven=[...rows,{id:'openglassResources',title:'Resources',installed:false,checked:false,operation:'install',applySupported:true,targetVersion:'2.0.0.115'}]
  .map(r=>r.id==='secureUxTheme'?{...r,installed:true,applySupported:false,operation:'repair'}:{...r,checked:false});
 send(seven,{locale});button().click();
 const actual=d.querySelectorAll('#components-plan-items li[data-product]').length;
 assert.equal(actual,4);assert.equal(d.querySelector('#components-count').textContent,locale==='ru'?'Изменений в плане: 4':'Planned changes: 4');
 assert.equal(d.querySelector('#product-secureUxTheme').checked,false);
}
// One common cleanup page: independent native IPC and optional product rows.
for(const locale of ['ru','en']){
 send([],{page:'full-cleanup',locale,cleanupPlatformEligible:true,cleanupOgEligible:true,cleanupUserData:false,cleanupOpenGlassConfig:false});
 const platform=d.getElementById('full-cleanup-choice'),og=d.getElementById('og-cleanup-choice');
 assert.equal(platform.parentElement.hidden,false);assert.equal(og.parentElement.hidden,false);
 before=messages.length;platform.click();assert.equal(platform.checked,true);assert.equal(og.checked,false);
 og.click();assert.equal(og.checked,true);platform.click();assert.equal(platform.checked,false);
 assert.deepEqual(messages.slice(before),['cleanup:platform:1','cleanup:og:1','cleanup:platform:0']);
 send([],{page:'full-cleanup',locale,cleanupPlatformEligible:false,cleanupOgEligible:true,cleanupUserData:false,cleanupOpenGlassConfig:true});
 assert.equal(platform.parentElement.hidden,true);assert.equal(og.parentElement.hidden,false);assert.equal(og.checked,true);
}
send([],{page:'cancelled',cancellationIsRemoval:true,cancelledDetail:''});
assert.match(d.getElementById('cancelled-title').textContent,/Удаление отменено|Removal was cancelled/);
assert.equal(d.getElementById('cancelled-detail').hidden,true);
send([],{page:'cancelled',cancellationIsRemoval:true,cancelledDetail:'Restored hosts'});
assert.equal(d.getElementById('cancelled-detail').hidden,false);
assert.equal(d.getElementById('cancelled-detail').textContent,'Restored hosts');
assert.deepEqual(errors,[]);dom.window.close();console.log('PASS: fresh install recommendations, repeat host echo, loading/capability gates and changed-helper recommendations');
