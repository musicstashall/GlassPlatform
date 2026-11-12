// Actual production HTML: the seventh card, capabilities and standard plan IPC.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
let receive;const messages=[],errors=[];const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8'),{
 url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(t,cb){receive=cb;},postMessage(m){messages.push(m);}}};}
});
const d=dom.window.document;
const rows=['x64','x86','settings64','settings32','openglass','secureUxTheme','openglassResources'].map(id=>({
 id,title:id,installed:true,checked:false,operation:'repair',applySupported:true,removeSupported:true,
 installedPackageVersion:'1.0.0',targetVersion:'1.0.0',remoteVersion:'1.0.0'}));
const send=(components,locale='ru',downloads=[])=>receive({data:{type:'state',appMode:'setup',page:'components',locale,components,downloads,componentsLoading:false}});
const input=()=>d.getElementById('product-openglassResources');
const available=()=>d.querySelector('#components-grid [data-product="openglassResources"]');
for(const locale of ['ru','en']){
 const fresh=rows.map(row=>row.id==='openglassResources'?{...row,installed:false,operation:'install',applySupported:false,
  applyHint:locale==='ru'?'Сначала установите OpenGlass':'Install OpenGlass first'}:row);
 send(fresh,locale);
 assert.ok(available(),'catalogue must include the seventh card');
 assert.ok(available().querySelector('.comp-icon svg'),'graphics icon');
 assert.ok(available().textContent.includes(locale==='ru'?'Сначала':'Install OpenGlass'));
 assert.ok(available().querySelector('.comp-decision').disabled);
 assert.equal(d.querySelectorAll('.product-transfer').length,0,'no downloads at selection/discovery');
 // Existing official OG + verified source permits the ordinary apply IPC.
 const ready=fresh.map(row=>row.id==='openglassResources'?{...row,applySupported:true}:row);
 send(ready,locale);available().querySelector('.comp-decision').click();
 assert.equal(messages.at(-1),'component:openglassResources:1');assert.ok(input().checked);
 assert.ok(input().closest('#components-installed-grid'));
 send(ready.map(row=>({...row,checked:row.id==='openglassResources'})),locale,[{id:'openglassResources',phase:'downloading',bytes:25,total:100,network:true}]);
 assert.match(d.querySelector('#components-plan-items li[data-product="openglassResources"]').textContent,/25%/);
 d.querySelector('#components-plan-items li[data-product="openglassResources"] .plan-cancel').click();
 assert.equal(messages.at(-1),'component:openglassResources:0');
 // Fresh pair: prerequisite is explicit, not silently selected by Resources.
 const empty=rows.map(row=>row.id==='openglass'||row.id==='openglassResources'?{...row,installed:false,operation:'install',applySupported:row.id==='openglass'}:row);
 send(empty,locale);assert.ok(available().querySelector('.comp-decision').disabled);
 d.querySelector('#components-grid [data-product="openglass"] .comp-decision').click();
 assert.equal(messages.at(-1),'component:openglass:1');
 send(empty.map(row=>row.id==='openglass'?{...row,checked:true}:row.id==='openglassResources'?{...row,applySupported:true}:row),locale);
 available().querySelector('.comp-decision').click();assert.equal(messages.at(-1),'component:openglassResources:1');
 assert.deepEqual([...d.querySelectorAll('#components-plan-items li[data-product]')].map(node=>node.dataset.product),['openglass','openglassResources']);
 // Installed/orphan resources remain removable without a usable OG Apply.
 const installed=rows.map(row=>row.id==='openglassResources'?{...row,applySupported:false}:row);
 send(installed,locale);input().closest('.c-card').querySelector('.comp-remove').click();
 assert.equal(messages.at(-1),'component:openglassResources:remove');
 send(installed.map(row=>({...row,checked:row.id==='openglassResources',operation:row.id==='openglassResources'?'remove':row.operation})),locale);
 assert.ok(d.querySelector('#components-plan-items li.remove[data-product="openglassResources"]'));
 // x86 capability hides both authoritative card and catalogue alias.
 send(rows.map(row=>({...row,hidden:row.id==='openglassResources'})),locale);
 assert.equal(input(),null);assert.equal(available(),null);
 // Once the host advertises the combined lane, recommendations include both.
 send(ready.map(row=>row.id==='openglass'?{...row,operation:'update',updateAvailable:true,targetVersion:'2.0.0',targetPayloadVersion:'2.0.0',installedPayloadVersion:'1.0.0'}:row),locale);
 const before=messages.length;d.getElementById('components-select-all').click();
 assert.ok(messages.slice(before).includes('component:openglass:1'));
 assert.ok(messages.slice(before).includes('component:openglassResources:1'));
}
assert.deepEqual(errors,[]);dom.window.close();
console.log('PASS: production RU/EN Resources card, icon, prerequisite refusal, apply/remove/undo IPC, progress, x86 hiding and compatible recommendations');
