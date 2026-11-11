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
 assert.equal(messages.at(-1),'component:'+row.id+':remove');assert.ok(d.querySelector('#components-grid').contains(card));assert.ok(card.classList.contains('active'));assert.equal(d.querySelector('#components-installed-grid [data-product="'+row.id+'"]'),null);
 // Native host echoes the explicit removal intent, not an unchecked Keep or repair.
 send(rows.map(x=>x.id===row.id?{...x,checked:true,operation:'remove'}:x));
 card=d.querySelector('#product-'+row.id).closest('.c-card');assert.equal(card.dataset.catalogState,'available');assert.ok(d.querySelector('#components-grid').contains(card));assert.ok(card.classList.contains('active'));
 assert.match(d.querySelector('#components-plan-items').textContent,/Удалить/);
 assert.equal(card.querySelector('.comp-decision').dataset.componentAction,'install');
 assert.equal(dom.window.eval('componentDropValue')({id:row.id,input:card.querySelector('input'),source:'available'},'installed'),true);
 // Undo Keep stays explicit in the plan; it restores the original installed state.
 d.querySelector('.plan-cancel').click();assert.equal(messages.at(-1),'component:'+row.id+':0');
 assert.ok(d.querySelector('#components-installed-grid').contains(card));
 // Selecting the available removed product replaces Remove with Apply.
 send(rows.map(x=>x.id===row.id?{...x,checked:true,operation:'remove'}:x));
 d.querySelector('#product-'+row.id).closest('.c-card').querySelector('.comp-decision').click();
 assert.equal(messages.at(-1),'component:'+row.id+':1');
 assert.ok(d.querySelector('#components-installed-grid #product-'+row.id));
}
// Residual entity: Installed + damaged, not a fresh dashed installation.
for(const checked of [false,true]){
 const residual={...rows.at(-1),checked,broken:true,installedVersion:'',targetVersion:'2.0.0'};
 send([residual]);const input=d.querySelector('#product-openglass'),card=input.closest('.c-card');
 assert.ok(d.querySelector('#components-installed-grid').contains(card));
 assert.equal(card.classList.contains('pending-install'),false);
 if(!checked)assert.ok(card.classList.contains('is-broken'));
 const drop=dom.window.eval('componentDropValue')({id:'openglass',input,source:'installed'},'available');
 assert.equal(drop,checked?false:'remove');
 dom.window.eval('commitComponentRemoval')(input);
 assert.equal(messages.at(-1),'component:openglass:remove');
}
// Fresh planned installation still returns to Available as deselection.
send([{...rows[0],installed:false,checked:true}]);
assert.equal(dom.window.eval('componentDropValue')({id:'x64',input:d.querySelector('#product-x64'),source:'installed'},'available'),false);
send(rows.map(x=>({...x,removeSupported:false})));assert.equal(d.querySelectorAll('.comp-remove').length,0);
// Bulk removal is only the same per-row plan intent; no engine/cleanup command.
send(rows);let beforeBulk=messages.length;d.querySelector('#components-remove-all').click();
assert.deepEqual(messages.slice(beforeBulk),rows.map(x=>'component:'+x.id+':remove'));
assert.equal(d.querySelectorAll('#components-installed-grid .c-card').length,0);
assert.equal(d.querySelectorAll('#components-plan-items li.remove').length,5);
assert.equal(d.querySelector('#components-remove-all').disabled,true);
beforeBulk=messages.length;d.querySelector('#components-remove-all').click();assert.equal(messages.length,beforeBulk);
// Locked/unsupported rows stay installed; pending fresh Apply is cancelled.
send([...rows.map((x,i)=>({...x,removeSupported:i!==1,disabled:i===2})),{id:'fresh',title:'Fresh',installed:false,checked:true,operation:'install'}]);
beforeBulk=messages.length;d.querySelector('#components-remove-all').click();
assert.deepEqual(messages.slice(beforeBulk),['component:fresh:0','component:x64:remove','component:settings32:remove','component:openglass:remove']);
assert.equal(d.querySelector('#product-x86').checked,false);assert.equal(d.querySelector('#product-settings64').checked,false);
assert.equal(d.querySelector('#product-fresh').checked,false);
send(rows.map(x=>({...x,removeSupported:false})));assert.equal(d.querySelector('#components-remove-all').disabled,true);
receive({data:{type:'state',appMode:'setup',page:'components',components:rows,componentsLoading:true}});
assert.equal(d.querySelector('#components-remove-all').disabled,true);
receive({data:{type:'state',appMode:'unins',page:'uninstall-select',components:rows,componentsLoading:false}});
beforeBulk=messages.length;d.querySelector('#components-remove-all').click();assert.deepEqual(messages.slice(beforeBulk),['components:select-all']);
for(const locale of ['ru','en']){
 receive({data:{type:'state',appMode:'setup',page:'full-cleanup',locale,cleanupBody:'Explicit full-removal review',cleanupUserData:false}});
 const option=d.querySelector('#full-cleanup-option'),choice=d.querySelector('#full-cleanup-choice');
 assert.equal(option.hidden,false);assert.equal(choice.checked,false);
 assert.match(d.querySelector('#confirm-body').textContent,/Explicit full-removal review/);
 choice.click();assert.equal(messages.at(-1),'cleanup:1');
 receive({data:{type:'state',cleanupUserData:true}});assert.equal(choice.checked,true);
 receive({data:{type:'state',page:'components'}});assert.equal(option.hidden,true);
}
assert.deepEqual(errors.map(x=>x.message),[]);dom.window.close();
process.stdout.write('PASS: five installed Glass cards: explicit removal IPC, host echo/plan, undo-to-Keep, capability refusal\n');
