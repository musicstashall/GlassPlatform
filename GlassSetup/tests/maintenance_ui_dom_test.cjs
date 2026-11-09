// Run with NODE_PATH pointing to an external installation of jsdom@26.
// Executes the actual production HTML; creates no duplicate/demo HTML asset.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
const errors=[],messages=[];let receive;
const console=new VirtualConsole();console.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(html,{url:'https://glassplatform.invalid/?lang=ru',runScripts:'dangerously',virtualConsole:console,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(type,callback){if(type==='message')receive=callback;},postMessage(message){messages.push(message);}}};}});
const d=dom.window.document;
const rows=[['x64','Platform x64'],['x86','Platform x86'],['settings64','GlassSettings x64'],['settings32','GlassSettings x86'],['openglass','OpenGlass'],['secureUxTheme','SecureUxTheme']].map(([id,title],i)=>({id,title,description:'Product purpose',checked:i===2,installed:i<3,installedVersion:i<3?'1.0.0.0':'',installedPackageVersion:i<3?'1.0.0.0':'',targetVersion:'1.1.0.0',operation:i<3?'update':'install',state:i===2?'Будет обновлён':'Без изменений',warning:i===1?'Architecture-specific warning':'',reasons:[]}));
assert.equal(typeof receive,'function');
receive({data:{type:'state',locale:'ru',page:'components',maintenance:true,components:rows}});
assert.match(d.querySelector('#components-title').textContent,/Продукты/);
assert.equal(d.querySelectorAll('#components-panel .c-card input.comp-check').length,6);
assert.equal(dom.window.getComputedStyle(d.querySelector('#components-grid')).display,'flex');
assert.equal(dom.window.getComputedStyle(d.querySelector('#components-panel .components-list')).height,'330px');
assert.equal(d.querySelectorAll('.c-card:not([data-catalog-alias]) .comp-version-line').length,6);
assert.equal(d.querySelectorAll('#components-panel .c-card:not([data-catalog-alias]).is-checked').length,1);
assert.equal(d.querySelector('#product-settings64').checked,true);
assert.equal(d.querySelector('#product-settings32').checked,false);
d.querySelector('#product-settings32').closest('.comp-card').querySelector('.comp-decision').click();
assert.equal(messages.at(-1),'component:settings32:1');
assert.equal(d.querySelector('#product-settings64').checked,true);
assert.equal(d.querySelector('#product-settings32').checked,true);
assert.equal(d.querySelectorAll('#components-panel .c-card:not([data-catalog-alias]).is-checked').length,2);
assert.match(d.querySelector('#components-count').textContent,/2 из 6/);
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').length,2);
// Catalogue mirrors share a single authoritative intent and do not duplicate counts.
assert.equal(d.querySelectorAll('#components-grid .c-card').length,6);
const alias=d.querySelector('[data-catalog-alias="settings64"]');assert.ok(alias);assert.equal(alias.querySelector('input'),null);
const beforeAlias=messages.length;alias.querySelector('.comp-decision').click();assert.equal(messages.length,beforeAlias);
assert.equal(d.querySelector('#product-settings64').checked,true);assert.ok(alias.classList.contains('idle'));
assert.equal(alias.querySelector('.c-chip'),null);assert.match(alias.querySelector('.c-src').textContent,/локально/);
assert.match(d.querySelector('#product-settings64').closest('.c-card').querySelector('.c-chip').textContent,/обновится до/);
d.querySelector('[data-shelf="available"] [data-view="needed"]').click();
assert.equal(d.querySelector('[data-shelf="available"] [data-view="needed"]').getAttribute('aria-pressed'),'true');
assert.equal(d.querySelector('[data-catalog-alias="settings64"]').hidden,true);
assert.equal(d.querySelector('[data-catalog-alias="settings32"]').hidden,true);
assert.equal(d.querySelector('#components-grid [data-catalog-alias="x86"]').hidden,false);
d.querySelector('[data-shelf="available"] [data-view="all"]').click();
// Warn popup is independent of product selection; dismissing it does not cancel.
const warn=d.querySelector('.comp-warning-button');warn.click();assert.equal(d.querySelector('#components-warning-dialog').hidden,false);
const before=messages.length;
d.querySelector('#components-warning-close').dispatchEvent(new dom.window.KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
assert.equal(d.querySelector('#components-warning-dialog').hidden,true);assert.equal(messages.length,before);
// Warnings live only on cards/popups, never in the plan (including mirror).
assert.equal(d.querySelectorAll('#components-plan-items .plan-warning').length,0);
assert.doesNotMatch(d.querySelector('#components-plan-items').textContent,/Architecture-specific warning/);
// Plan contains only a brief notice summary; full text belongs to Details.
assert.equal(d.querySelector('#components-plan-notices').hidden,false);
assert.doesNotMatch(d.querySelector('#components-plan-notices').textContent,/Architecture-specific warning/);
d.querySelector('#components-plan-notices button').click();
assert.match(d.querySelector('#components-warning-text').textContent,/Architecture-specific warning/);
d.querySelector('#components-warning-ok').click();
assert.equal(d.querySelector('#components-warning-dialog').hidden,true);
assert.equal(d.activeElement,d.querySelector('#components-plan-notices button'));

for(const card of d.querySelectorAll('[data-product="x86"]')){
 const triangle=card.querySelector('.comp-warning-button');assert.ok(triangle);
 assert.equal(triangle.parentElement.className,'c-card-top');assert.ok(triangle.querySelector('svg'));
 triangle.click();assert.equal(d.querySelector('#components-warning-text').textContent,'Architecture-specific warning');
 d.querySelector('#components-warning-close').click();assert.equal(d.activeElement,triangle);
}
// The cascade must not grey out idle newer Available cards; damage stays red.
const newerCard=d.querySelector('[data-catalog-alias="x86"]');assert.ok(newerCard.classList.contains('has-newer'));
assert.equal(dom.window.getComputedStyle(newerCard).borderTopColor,'rgba(131, 217, 166, 0.35)');
assert.match(dom.window.getComputedStyle(newerCard).background,/#1f3a34|31, 58, 52/);
receive({data:{type:'state',page:'components',components:rows.map(r=>({...r,checked:false,broken:r.id==='x86'}))}});
assert.ok(d.querySelector('[data-catalog-alias="x86"]').classList.contains('is-broken'));
assert.equal(d.querySelector('[data-catalog-alias="x86"]').classList.contains('has-newer'),false);
assert.equal(d.querySelector('#product-x86').checked,false);
// Empty states occupy their list plane; checking copy contains no implementation tutorial.
receive({data:{type:'state',appMode:'setup',locale:'ru',page:'components',components:[]}});
for(const id of ['components-grid','components-installed-grid']){
 const grid=d.getElementById(id);assert.ok(grid.classList.contains('is-empty'));assert.ok(grid.closest('.c-lane-body').classList.contains('is-empty'));
 assert.equal(dom.window.getComputedStyle(grid.querySelector('.c-drop')).justifyContent,'center');
}
assert.equal(dom.window.getComputedStyle(d.querySelector('.components-plan-empty')).flexGrow,'1');
receive({data:{type:'state',page:'checking'}});assert.equal(d.querySelector('#checking-copy').textContent,'Ищем обновление GlassSetup.');
receive({data:{type:'state',locale:'en',page:'checking'}});assert.equal(d.querySelector('#checking-copy').textContent,'Looking for a GlassSetup update.');
// Vendor local ProductVersion has priority; release-tag metadata is labelled.
for(const local of [true,false]){
 const sutOffer={id:'secureUxTheme',title:'SecureUxTheme',installed:false,checked:false,operation:'install',targetVersion:local?'4.0.0':'4.1.0',targetVersionKind:local?'msi':'release',localPackageVersion:local?'4.0.0':'',remoteVersion:'4.1.0',releaseTag:'v4.1.0'};
 receive({data:{type:'state',appMode:'setup',locale:'ru',page:'components',components:[sutOffer]}});
 const card=d.querySelector('#product-secureUxTheme').closest('.c-card');
 assert.equal(card.querySelector('.comp-version-line').textContent,local?'4.0.0':'4.1.0');
 assert.match(card.querySelector('.c-src').textContent,/на сервере 4.1.0/);
 if(local)assert.match(card.querySelector('.c-src').textContent,/локально 4.0.0/);
 else assert.doesNotMatch(card.querySelector('.c-src').textContent,/пакет/);
}
// English state must regenerate decision labels even when product metadata is unchanged.
receive({data:{type:'state',locale:'en',page:'components',maintenance:false,components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'GlassPlatform products');
assert.equal(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').title,'Undo update');
receive({data:{type:'state',appMode:'unins',page:'uninstall-select',components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'Remove products');
assert.equal(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').title,'Keep installed');
// Cancelling a planned row must use the same real checkbox IPC and move a fresh product back.
receive({data:{type:'state',appMode:'setup',page:'components',components:rows.map(r=>({...r,checked:r.id==='settings32'}))}});
const fresh=d.querySelector('#components-installed-grid #product-settings32');assert.ok(fresh);
d.querySelector('.plan-cancel').click();assert.equal(messages.at(-1),'component:settings32:0');
assert.ok(d.querySelector('#components-grid #product-settings32'));
// A disabled product has no actionable shortcut around the checkbox gate.
receive({data:{type:'state',appMode:'setup',page:'components',components:rows.map(r=>({...r,disabled:r.id==='settings32'}))}});
assert.equal(d.querySelector('#product-settings32').closest('.comp-card').querySelector('.comp-decision').disabled,true);
// Real Setup removal capability is opt-in and distinct from an install checkbox.
const sutRows=rows.map(r=>({...r,checked:false,installed:r.id==='secureUxTheme',installedVersion:'4.0.0',operation:'repair',disabled:false,removeSupported:r.id==='secureUxTheme',applySupported:r.id!=='secureUxTheme'}));
receive({data:{type:'state',appMode:'setup',page:'components',locale:'ru',components:sutRows}});
let sut=d.querySelector('#product-secureUxTheme'),sutCard=sut.closest('.c-card');
assert.equal(sutCard.querySelector('.comp-decision').disabled,true,'unsupported vendor repair must not look actionable');
const beforeUnsupported=messages.length;sut.checked=true;sut.dispatchEvent(new dom.window.Event('change',{bubbles:true}));
assert.equal(sut.checked,false);assert.equal(messages.length,beforeUnsupported);
assert.ok(sutCard.querySelector('.comp-remove'));const beforeRemove=messages.length;
sutCard.querySelector('.comp-remove').click();
assert.equal(messages.length,beforeRemove+1);assert.equal(messages.at(-1),'component:secureUxTheme:remove');
assert.equal(sut.checked,true);assert.ok(d.querySelector('#components-grid').contains(sutCard));assert.ok(sutCard.classList.contains('active'));
assert.match(d.querySelector('#components-plan-items li.remove').textContent,/Удалить/);
assert.equal(d.querySelector('#product-settings64').checked,false);
dom.window.eval('commitComponentRemoval')(sut);assert.equal(messages.length,beforeRemove+1,'duplicate removal is a no-op');
const nativeRemove=sutRows.map(r=>({...r,checked:r.id==='secureUxTheme',operation:r.id==='secureUxTheme'?'remove':r.operation}));
receive({data:{type:'state',components:nativeRemove}});
sut=d.querySelector('#product-secureUxTheme');sutCard=sut.closest('.c-card');
assert.equal(sut.dataset.intent,'remove');assert.ok(d.querySelector('#components-grid').contains(sutCard));assert.ok(sutCard.classList.contains('active'));
assert.equal(sutCard.querySelector('.comp-decision').title,'Оставить установленным');
d.querySelector('#components-plan-items .plan-cancel').click();
assert.equal(messages.at(-1),'component:secureUxTheme:0');assert.equal(sut.checked,false);
assert.equal(sutCard.classList.contains('pending-remove'),false);
assert.ok(sutCard.contains(d.activeElement),'cancel must leave a keyboard focus target');
receive({data:{type:'state',components:sutRows.map(r=>({...r,removeSupported:false}))}});
assert.equal(d.querySelector('#product-secureUxTheme').closest('.c-card').querySelector('.comp-remove'),null);
// A blocked installed SUT retains an explanatory trash affordance, not a mutation.
for(const disabled of [false,true]){
 const reason='SecureUxTheme is active; safe active removal is not implemented.';
 receive({data:{type:'state',appMode:'setup',page:'components',components:sutRows.map(r=>({...r,disabled:r.id==='secureUxTheme'&&disabled,removeSupported:false,removeBlockedReason:r.id==='secureUxTheme'?reason:''}))}});
 const input=d.querySelector('#product-secureUxTheme'),card=input.closest('.c-card'),info=card.querySelector('.comp-remove.removal-info');
 assert.ok(info);assert.equal(info.disabled,false);
 const before=messages.length;info.click();assert.equal(messages.length,before);assert.equal(input.checked,false);
 assert.equal(d.querySelector('#components-warning-text').textContent,reason);
 d.querySelector('#components-warning-ok').click();assert.equal(d.activeElement,info);
 assert.equal(dom.window.componentDropText({id:'secureUxTheme',source:'installed',input},'available'),reason);
 assert.equal(d.querySelector('#components-remove-all').hidden,true);
 card.querySelector('.comp-name').click();assert.match(d.querySelector('#components-warning-text').textContent,/safe active removal/);
 d.querySelector('#components-warning-ok').click();
}
receive({data:{type:'state',appMode:'unins',page:'uninstall-select',components:sutRows}});
assert.equal(d.querySelector('#product-secureUxTheme').closest('.c-card').querySelector('.comp-remove'),null);
// Concept C content/state matrix, including warning-only healthy products.
const matrix=rows.map((r,i)=>({...r,warning:i===0?'Manual configuration':'',disabled:false,checked:i===3||i===5,
 installed:i!==3&&i!==4,installedVersion:i===3||i===4?'':'1.0.0.0',targetVersion:i===2||i===3||i===4?'1.1.0.0':'1.0.0.0',
 remoteVersion:i===2||i===3||i===4?'1.1.0.0':'',packageSize:16*1048576,
 operation:i===2?'update':i===3||i===4?'install':'repair',broken:i===1,badgeTone:i===1?'repair':'keep'}));
receive({data:{type:'state',locale:'ru',appMode:'setup',page:'components',components:matrix}});
const installedCard=id=>d.querySelector('#product-'+id).closest('.c-card');
const catalogueCard=id=>d.querySelector('#components-grid [data-product="'+id+'"]');
assert.equal(catalogueCard('x64').querySelector('.c-chip'),null);
assert.match(catalogueCard('x64').querySelector('.c-src').textContent,/локально 1.0.0.0/);
assert.equal(catalogueCard('x64').querySelector('.src-bit.remote'),null);
assert.ok(catalogueCard('x64').classList.contains('idle'));
assert.match(catalogueCard('settings64').querySelector('.c-meta').textContent,/1.0.0.0 → 1.1.0.0/);
assert.match(catalogueCard('settings64').querySelector('.src-bit.remote').textContent,/на сервере 1.1.0.0/);
assert.ok(catalogueCard('settings32').classList.contains('idle'));
assert.equal(catalogueCard('settings32').querySelector('.comp-decision').disabled,false);
assert.match(installedCard('settings64').querySelector('.c-chip').textContent,/1.1.0.0 в «Доступно»/);
assert.doesNotMatch(installedCard('settings64').querySelector('.c-meta').textContent,/→/);
assert.equal(installedCard('x64').querySelector('.c-chip.current').textContent,'1.0.0.0');
assert.match(installedCard('settings32').querySelector('.c-chip.pending').textContent,/Будет установлено/);
assert.match(installedCard('secureUxTheme').querySelector('.c-chip.download').textContent,/переустановится/);
assert.ok(installedCard('secureUxTheme').classList.contains('is-activated'));
assert.ok(installedCard('x86').classList.contains('is-broken'));
assert.doesNotMatch(installedCard('x86').querySelector('.c-meta').textContent,/SHA-256/,'do not invent a hash failure from a registry/unknown inspection failure');
for(const name of ['available','installed'])d.querySelector('[data-shelf="'+name+'"] [data-view="needed"]').click();
assert.equal(d.querySelectorAll('#components-grid .c-card:not([hidden])').length,3);
assert.equal(d.querySelectorAll('#components-installed-grid .c-card:not([hidden])').length,4);
assert.equal(catalogueCard('x64').hidden,true,'warning alone is not a product action');
assert.equal(catalogueCard('settings32').hidden,true,'already in plan must leave Available/needed');
assert.equal(installedCard('settings32').hidden,false,'pending install stays in Installed/needed');
// Labels are action-specific C labels, not a generic Keep/Skip/Undo toggle.
assert.equal(installedCard('settings64').querySelector('.comp-decision').title,'Обновить до 1.1.0.0');
assert.equal(installedCard('secureUxTheme').querySelector('.comp-decision').title,'Отменить переустановку');
assert.equal(installedCard('settings32').querySelector('.comp-decision').title,'Удалить');
assert.ok(installedCard('settings32').querySelector('.comp-decision').classList.contains('danger'));
assert.equal(catalogueCard('settings32').querySelector('.comp-decision').title,'Установить GlassSettings x86');
assert.equal(catalogueCard('settings32').querySelector('.comp-decision').classList.contains('danger'),false);
assert.equal(d.querySelector('#installed-title').textContent,'Установлено');
const cardName=catalogueCard('settings32').querySelector('.comp-name');
assert.equal(cardName.tagName,'SPAN');assert.equal(cardName.getAttribute('role'),'button');
cardName.dispatchEvent(new dom.window.KeyboardEvent('keydown',{key:'Enter',bubbles:true,cancelable:true}));
assert.equal(d.querySelector('#components-warning-dialog').hidden,false);
d.querySelector('#components-warning-close').click();
// Needed is the actionable subset of All, never a badge-tone proxy.
const filterRows=rows.map(r=>({...r,installed:true,checked:false,broken:false,operation:'repair',badgeTone:'repair',warning:'Warning only',applySupported:false,disabled:false,locked:false}));
receive({data:{type:'state',locale:'ru',appMode:'setup',page:'components',components:filterRows}});
for(const shelf of ['available','installed']){
 d.querySelector('[data-shelf="'+shelf+'"] [data-view="all"]').click();
 const grid=d.querySelector(shelf==='available'?'#components-grid':'#components-installed-grid');
 assert.equal(grid.querySelectorAll('.c-card:not([hidden])').length,6);
 if(shelf==='available')assert.equal(grid.querySelectorAll('.c-card.active').length,0);
 d.querySelector('[data-shelf="'+shelf+'"] [data-view="needed"]').click();
 assert.equal(grid.querySelectorAll('.c-card:not([hidden])').length,0);
}
// Disabled/locked/unsupported offers cannot leak into Available/needed.
filterRows[0]={...filterRows[0],installed:false,applySupported:true,disabled:true,operation:'install'};
filterRows[1]={...filterRows[1],installed:false,applySupported:true,locked:true,operation:'install'};
filterRows[2]={...filterRows[2],applySupported:true,operation:'update',targetVersion:'2.0.0'};
filterRows[3]={...filterRows[3],installed:false,applySupported:true,checked:true,operation:'install'};
filterRows[4]={...filterRows[4],applySupported:true,broken:true};
filterRows[5]={...filterRows[5],broken:true}; // Maintenance unsupported, not an executable offer.
receive({data:{type:'state',components:filterRows}});
assert.deepEqual([...d.querySelectorAll('#components-grid .c-card:not([hidden])')].map(c=>c.dataset.product),['settings64','openglass']);
assert.deepEqual([...d.querySelectorAll('#components-installed-grid .c-card:not([hidden])')].map(c=>c.dataset.product),['settings64','settings32','openglass']);
for(const shelf of ['available','installed'])d.querySelector('[data-shelf="'+shelf+'"] [data-view="all"]').click();
// No MSI or chosen action: inert SUT payloads must not produce Installed/removal.
const absentRows=rows.map(r=>({...r,installed:false,checked:false,operation:'install',installedVersion:'',installedPackageVersion:'',disabled:false,locked:false,applySupported:true,removeSupported:false,warning:'',presenceDetail:r.id==='secureUxTheme'?'Installed MSI inventory rc=0; installed=false; candidate state=2':''}));
receive({data:{type:'state',locale:'ru',appMode:'setup',page:'components',maintenance:true,components:absentRows}});
const absentSut=d.querySelector('#product-secureUxTheme');
assert.equal(absentSut.checked,false);assert.ok(d.querySelector('#components-grid').contains(absentSut));
assert.equal(absentSut.closest('.c-card').dataset.catalogState,'available');
assert.ok(absentSut.closest('.c-card').classList.contains('active'));
assert.equal(absentSut.closest('.c-card').querySelector('.comp-decision').disabled,false);
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').length,0);
absentSut.closest('.c-card').querySelector('.comp-name').click();
assert.doesNotMatch(d.querySelector('#components-warning-dialog').textContent,/Installed MSI inventory|candidate state=2/);
assert.equal(dom.window.getComputedStyle(d.querySelector('.components-warning-shade footer')).position,'static');
d.querySelector('#components-warning-close').click();
const beforeAbsent=messages.length;absentSut.closest('.c-card').querySelector('.comp-decision').click();
assert.equal(messages.length,beforeAbsent+1);assert.equal(messages.at(-1),'component:secureUxTheme:1');
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').length,1);
assert.equal(d.querySelector('[data-catalog-alias="secureUxTheme"]').dataset.catalogState,'planned');
absentSut.closest('.c-card').querySelector('.comp-decision').click();
assert.equal(messages.at(-1),'component:secureUxTheme:0');assert.ok(d.querySelector('#components-grid').contains(absentSut));
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').length,0);
// MSI package versions win over obsolete PE/bundle display fields. Available
// shows its own candidate, not the installed version copied onto both panels.
const versions=rows.map((r,i)=>({...r,checked:false,installed:i<5,operation:'repair',disabled:false,
 remotePayloadVersion:r.id==='openglass'?'3.0.2.3749':undefined,installedPayloadVersion:r.id==='openglass'?'1.5.0':undefined,targetPayloadVersion:r.id==='openglass'?'1.6.0':undefined,
 installedVersion:'99.0.0.999',installedPackageVersion:(i+1)+'.2.3',targetVersion:(i+2)+'.4.5',
 updateAvailable:i===0,broken:i===0,applySupported:true}));
receive({data:{type:'state',page:'components',appMode:'setup',locale:'en',components:versions}});
for(let i=0;i<5;i++){
 const card=d.querySelector('#product-'+rows[i].id).closest('.c-card');
 assert.match(card.querySelector('.c-meta').textContent,new RegExp(rows[i].id==='openglass'?'1\\.5\\.0':(i+1)+'\\.2\\.3'));
 assert.doesNotMatch(card.querySelector('.c-meta').textContent,/99\.0\.0/);
 const offer=d.querySelector('[data-catalog-alias="'+rows[i].id+'"]');
 assert.ok(offer);assert.ok(offer.querySelector('.c-meta').textContent.includes(rows[i].id==='openglass'?'1.6.0':(i+2)+'.4.5'));
}
assert.match(d.querySelector('[data-catalog-alias="openglass"] .c-src').textContent,/local 1\.5\.0/);
assert.match(d.querySelector('[data-catalog-alias="openglass"] .c-src').textContent,/on server 3\.0\.2\.3749/);
assert.ok(d.querySelector('#product-x64').closest('.c-card').classList.contains('is-broken'));
assert.equal(d.querySelector('[data-catalog-alias="x64"] .comp-decision').title,'Update to 2.4.5');
receive({data:{type:'state',components:[{...versions[2],installed:true,installedVersion:'99.0.0',installedPackageVersion:'',targetVersion:'',applySupported:false}]}});
assert.equal(d.querySelector('#product-settings64').closest('.c-card').querySelector('.c-meta').textContent,'—');
receive({data:{type:'state',components:[{...versions[5],installed:false,installedVersion:'',installedPackageVersion:'',targetVersion:''}]}});
assert.equal(d.querySelector('#product-secureUxTheme').closest('.c-card').querySelector('.c-meta').textContent,'—');
receive({data:{type:'state',page:'checking',checkingStatus:'Downloading and verifying new Setup'}});
assert.equal(d.querySelector('#checking-status').textContent,'Downloading and verifying new Setup');
assert.equal(d.querySelector('#update-panel'),null);
assert.deepEqual(errors.map(e=>e.message),[]);
// Recommendations are not "select every product": only verified need/explicit recommendation.
const healthy=rows.map(r=>({...r,installed:true,checked:false,broken:false,operation:'repair',updateAvailable:false,warning:'',disabled:false,locked:false,applySupported:true,removeSupported:false}));
receive({data:{type:'state',locale:'ru',page:'components',appMode:'setup',components:healthy}});
assert.equal(d.querySelector('#components-select-all').disabled,true);
assert.equal(d.querySelector('#components-remove-all').hidden,true);
const recommended=healthy.map(r=>({...r}));
recommended[0].broken=true;
recommended[2].installed=false;recommended[2].operation='install';
recommended[3].operation='update';recommended[3].updateAvailable=true;
recommended[4].broken=true;recommended[4].locked=true;
recommended[5].broken=true;recommended[5].applySupported=false;
receive({data:{type:'state',locale:'ru',page:'components',appMode:'setup',components:recommended}});
assert.equal(d.querySelector('#components-select-all').disabled,false);
const start=messages.length;d.querySelector('#components-select-all').click();
assert.deepEqual(messages.slice(start),['component:x64:1','component:settings32:1']);
assert.equal(d.querySelector('#product-x86').checked,false,'healthy products remain manual');
assert.equal(d.querySelector('#product-settings64').checked,false,'absent products are not implicit recommendations');
assert.equal(d.querySelector('#components-select-all').disabled,true);
assert.equal(d.querySelector('#components-select-all').textContent,'Всё выбрано');
d.querySelector('#product-x64').checked=false;d.querySelector('#product-x64').dispatchEvent(new dom.window.Event('change',{bubbles:true}));
assert.match(d.querySelector('#components-select-all').textContent,/Принять все рекомендации/);
assert.equal(d.querySelector('#components-select-all').disabled,false);
assert.equal(errors.length,0);
// All six product rows share exactly server/local source order in RU and EN.
for(const lang of ['ru','en'])for(const installed of [false,true]){
 const sourceRows=rows.map(r=>({...r,installed,checked:false,disabled:false,locked:false,applySupported:true,
  operation:installed?'repair':'install',installedPackageVersion:'1.0.0',installedPayloadVersion:r.id==='openglass'?'1.0.0':undefined,
  targetVersion:'2.0.0',targetPayloadVersion:r.id==='openglass'?'2.0.0':undefined,
  localPackageVersion:'2.0.0',remoteVersion:'3.0.0',remotePayloadVersion:r.id==='openglass'?'3.0.0':undefined,releaseTag:'v3.0.0'}));
 receive({data:{type:'state',locale:lang,page:'components',appMode:'setup',components:sourceRows}});
 for(const row of sourceRows){
  const card=installed?d.querySelector('[data-catalog-alias="'+row.id+'"]'):d.querySelector('#product-'+row.id).closest('.c-card');
  const bits=[...card.querySelectorAll('.c-src .src-bit')];assert.equal(bits.length,2);
  assert.equal(bits[0].textContent,(lang==='ru'?'на сервере ':'on server ')+'3.0.0');
  assert.equal(bits[1].textContent,(lang==='ru'?'локально ':'local ')+(installed?'1.0.0':'2.0.0'));
  assert.doesNotMatch(card.querySelector('.c-src').textContent,/пакет|релиз|package|release/i);
 }
}
receive({data:{type:'state',locale:'ru',page:'components',components:[{id:'x64',title:'Platform',installed:false,targetVersion:'2.0.0',operation:'install',applySupported:true}]}});
assert.equal(d.querySelector('#product-x64').closest('.c-card').querySelectorAll('.c-src .src-bit').length,0,'unknown origins are not invented from target version');
for(const lang of ['ru','en']) {
 receive({data:{type:'state',locale:lang,page:'error',ui:{},errorReason:'SecureUxTheme removal failed'}});
 assert.equal(d.querySelector('#error-title').textContent,lang==='ru'?'Не удалось выполнить операции':'Operations could not be completed');
 assert.doesNotMatch(d.querySelector('#error-body').textContent,/установк|installation/i);
 assert.equal(d.querySelector('#error-detail').textContent,'SecureUxTheme removal failed');
}
dom.window.close();
process.stdout.write('PASS: separate catalogue/composition content, planned catalogue idle/needed exclusion; actual production DOM: six rows, real per-architecture IPC, version metadata, live plan/counts, warnings/Escape, RU/EN, setup/uninstall titles and disabled-action gate\n');
