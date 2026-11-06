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
assert.match(d.querySelector('#components-title').textContent,/Обслуживание/);
assert.equal(d.querySelectorAll('.comp-decision').length,6);
assert.equal(dom.window.getComputedStyle(d.querySelector('#components-grid')).gridTemplateColumns,'1fr');
assert.equal(dom.window.getComputedStyle(d.querySelector('#components-panel .components-list')).height,'330px');
assert.equal(d.querySelectorAll('.comp-version-line').length,6);
assert.equal(d.querySelectorAll('#components-grid .comp-card.is-checked').length,1);
assert.equal(d.querySelector('#product-settings64').checked,true);
assert.equal(d.querySelector('#product-settings32').checked,false);
d.querySelector('#product-settings32').closest('.comp-card').querySelector('.comp-decision').click();
assert.equal(messages.at(-1),'component:settings32:1');
assert.equal(d.querySelector('#product-settings64').checked,true);
assert.equal(d.querySelector('#product-settings32').checked,true);
assert.equal(d.querySelectorAll('#components-grid .comp-card.is-checked').length,2);
assert.match(d.querySelector('#components-count').textContent,/2 из 6/);
assert.equal(d.querySelectorAll('#components-plan-items li:not(.plan-warning)').length,2);
// Warn popup is independent of product selection; dismissing it does not cancel.
const warn=d.querySelector('.comp-warning-button');warn.click();assert.equal(d.querySelector('#components-warning-dialog').hidden,false);
const before=messages.length;
d.querySelector('#components-warning-close').dispatchEvent(new dom.window.KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
assert.equal(d.querySelector('#components-warning-dialog').hidden,true);assert.equal(messages.length,before);
// English state must regenerate decision labels even when product metadata is unchanged.
receive({data:{type:'state',locale:'en',page:'components',maintenance:false,components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'Install GlassPlatform');
assert.match(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').textContent,/Update/);
receive({data:{type:'state',appMode:'unins',page:'uninstall-select',components:rows}});
assert.equal(d.querySelector('#components-title').textContent,'Remove products');
assert.match(d.querySelector('#product-settings64').closest('.comp-card').querySelector('.comp-decision').textContent,/Remove/);
// A disabled product has no actionable shortcut around the checkbox gate.
receive({data:{type:'state',appMode:'setup',page:'components',components:rows.map(r=>({...r,disabled:r.id==='settings32'}))}});
assert.equal(d.querySelector('#product-settings32').closest('.comp-card').querySelector('.comp-decision').disabled,true);
assert.deepEqual(errors.map(e=>e.message),[]);
dom.window.close();
process.stdout.write('PASS: actual production DOM: six rows, real per-architecture IPC, version metadata, live plan/counts, warnings/Escape, RU/EN, setup/uninstall titles and disabled-action gate\n');
