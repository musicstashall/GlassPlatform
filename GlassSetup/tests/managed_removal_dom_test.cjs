const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
let receive;const messages=[],errors=[];const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(html,{url:'https://glassplatform.invalid/?lang=ru',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(t,cb){if(t==='message')receive=cb;},postMessage(m){messages.push(m);}}};}});
const d=dom.window.document;
const rows=['x64','x86','settings64','settings32','openglass'].map(id=>({id,title:id,description:'Installed',installed:true,checked:false,operation:'repair',removeSupported:true,installedVersion:'1.0.0',targetVersion:'1.0.0'}));
const send=components=>receive({data:{type:'state',page:'components',maintenance:true,components,componentsLoading:false}});
send(rows);assert.equal(d.querySelectorAll('#components-installed-grid .comp-remove').length,5);
for(const row of rows){
 send(rows);let card=d.querySelector('#product-'+row.id).closest('.c-card');card.querySelector('.comp-remove').click();
 assert.equal(messages.at(-1),'component:'+row.id+':remove');assert.match(card.getAttribute('aria-label'),/удаление/);
 // Native host echoes the explicit removal intent, not an unchecked Keep or repair.
 send(rows.map(x=>x.id===row.id?{...x,checked:true,operation:'remove'}:x));
 card=d.querySelector('#product-'+row.id).closest('.c-card');assert.equal(card.dataset.catalogState,'removal-planned');
 assert.match(d.querySelector('#components-plan-items').textContent,/Удалить/);
 assert.equal(card.querySelector('.comp-decision').dataset.componentAction,'restore');
 card.querySelector('.comp-decision').click();assert.equal(messages.at(-1),'component:'+row.id+':0');
}
send(rows.map(x=>({...x,removeSupported:false})));assert.equal(d.querySelectorAll('.comp-remove').length,0);
assert.deepEqual(errors.map(x=>x.message),[]);dom.window.close();
process.stdout.write('PASS: five installed Glass cards: explicit removal IPC, host echo/plan, undo-to-Keep, capability refusal\n');
