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
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning)').length,2);
// Catalogue mirrors share a single authoritative intent and do not duplicate counts.
assert.equal(d.querySelectorAll('#components-grid .c-card').length,6);
const alias=d.querySelector('[data-catalog-alias="settings64"]');assert.ok(alias);assert.equal(alias.querySelector('input'),null);
alias.querySelector('.comp-decision').click();assert.equal(messages.at(-1),'component:settings64:0');
assert.equal(d.querySelector('#product-settings64').checked,false);
d.querySelector('[data-shelf="available"] [data-view="needed"]').click();
assert.equal(d.querySelector('[data-shelf="available"] [data-view="needed"]').getAttribute('aria-pressed'),'true');
d.querySelector('[data-shelf="available"] [data-view="all"]').click();
// Warn popup is independent of product selection; dismissing it does not cancel.
const warn=d.querySelector('.comp-warning-button');warn.click();assert.equal(d.querySelector('#components-warning-dialog').hidden,false);
const before=messages.length;
d.querySelector('#components-warning-close').dispatchEvent(new dom.window.KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
assert.equal(d.querySelector('#components-warning-dialog').hidden,true);assert.equal(messages.length,before);
// English state must regenerate decision labels even when product metadata is unchanged.
receive({data:{type:'state',locale:'en',page:'components',maintenance:false,components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'GlassPlatform products');
assert.match(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').textContent,/Update/);
receive({data:{type:'state',appMode:'unins',page:'uninstall-select',components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'Remove products');
assert.match(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').textContent,/Remove/);
// Cancelling a planned row must use the same real checkbox IPC and move a fresh product back.
receive({data:{type:'state',appMode:'setup',page:'components',components:rows.map(r=>({...r,checked:r.id==='settings32'}))}});
const fresh=d.querySelector('#components-installed-grid #product-settings32');assert.ok(fresh);
d.querySelector('.plan-cancel').click();assert.equal(messages.at(-1),'component:settings32:0');
assert.ok(d.querySelector('#components-grid #product-settings32'));
// A disabled product has no actionable shortcut around the checkbox gate.
receive({data:{type:'state',appMode:'setup',page:'components',components:rows.map(r=>({...r,disabled:r.id==='settings32'}))}});
assert.equal(d.querySelector('#product-settings32').closest('.comp-card').querySelector('.comp-decision').disabled,true);
assert.deepEqual(errors.map(e=>e.message),[]);
dom.window.close();
process.stdout.write('PASS: actual production DOM: six rows, real per-architecture IPC, version metadata, live plan/counts, warnings/Escape, RU/EN, setup/uninstall titles and disabled-action gate\n');
