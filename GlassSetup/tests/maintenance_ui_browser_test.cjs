// Real Chromium layout checks against the approved concept C, using production HTML.
// Optional external deps: playwright-core; CHROMIUM_PATH or @sparticuz/chromium.
// No Glass executable/service is launched; browser IPC/discovery is a fixture.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright-core');
const root=path.resolve(__dirname,'../..');
async function cardContentSpec(page,selector){
 return page.locator(selector).evaluate(card=>{
  const meta=card.querySelector('.c-meta'),foot=card.querySelector('.c-foot'),button=foot.querySelector('.c-move'),chip=foot.querySelector('.c-chip,.c-src');
  const css=getComputedStyle(button),m=getComputedStyle(meta);
  return {meta:meta.textContent,mark:chip.textContent,
   states:['active','idle','pending-install','pending-remove','is-activated','has-offer','has-newer','is-broken'].filter(c=>card.classList.contains(c)),
   title:button.title,aria:button.getAttribute('aria-label'),svg:button.querySelector('svg').outerHTML,
   buttonKind:['danger','repair','is-on'].filter(c=>button.classList.contains(c)),
   buttonWidth:css.width,buttonHeight:css.height,iconWidth:getComputedStyle(button.querySelector('svg')).width,
   buttonCount:foot.querySelectorAll('.c-move').length,productIconColor:getComputedStyle(card.querySelector('.comp-icon,.c-icon')).color,nameFont:getComputedStyle(card.querySelector('.c-name')).fontSize,
   metaInset:Math.round(meta.getBoundingClientRect().left-card.getBoundingClientRect().left),metaPadding:m.paddingLeft,metaFont:m.fontSize};
 });
}
async function neededFilterParity(page,products){
 const rows=products.map(r=>({...r,installed:true,checked:false,broken:false,operation:'repair',badgeTone:'repair',warning:'Warning only',applySupported:false,disabled:false,locked:false}));
 const seed=async()=>page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',appMode:'setup',page:'components',components}}),rows);
 const view=async(shelf,mode)=>page.locator(`[data-shelf="${shelf}"] [data-view="${mode}"]`).click();
 await seed();
 for(const shelf of ['available','installed']){
  const grid=page.locator(shelf==='available'?'#components-grid':'#components-installed-grid');
  await view(shelf,'all');assert.equal(await grid.locator('.c-card:visible').count(),6);
  if(shelf==='available')assert.equal(await grid.locator('.c-card.active').count(),0);
  await view(shelf,'needed');assert.equal(await grid.locator('.c-card:visible').count(),0);
 }
 rows[0]={...rows[0],installed:false,applySupported:true,disabled:true,operation:'install'};
 rows[1]={...rows[1],installed:false,applySupported:true,locked:true,operation:'install'};
 rows[2]={...rows[2],applySupported:true,operation:'update',targetVersion:'2.0.0'};
 rows[3]={...rows[3],installed:false,applySupported:true,checked:true,operation:'install'};
 rows[4]={...rows[4],applySupported:true,broken:true};
 rows[5]={...rows[5],broken:true};
 await seed();
 const ids=selector=>page.locator(selector+' .c-card:visible').evaluateAll(cards=>cards.map(c=>c.dataset.product));
 assert.deepEqual(await ids('#components-grid'),['settings64','openglass']);
 assert.deepEqual(await ids('#components-installed-grid'),['settings64','settings32','openglass']);
 // Filter changes are view-only, not native install/remove requests.
 const before=await page.evaluate(()=>window.__messages.length);
 for(const shelf of ['available','installed'])await view(shelf,'all');
 assert.equal(await page.evaluate(()=>window.__messages.length),before);
 console.log('PASS: both Needed filters: zero actionable offers means empty; no warning-tone false damage, disabled/locked/unsupported offers; update/repair and pending install retained; no filter IPC.');
}
async function freshInstallReturn(page,products,out){
 const fresh=products.map(r=>({...r,installed:false,installedVersion:'',installedPackageVersion:'',operation:'install',badgeTone:'install',checked:r.id!=='secureUxTheme',locked:false,disabled:false,warning:''}));
 await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',appMode:'setup',maintenance:false,page:'components',components}}),fresh);
 const initial=await page.evaluate(()=>window.__messages.length);
 const card=page.locator('#product-settings32').locator('..');await card.scrollIntoViewIfNeeded();
 const title=await card.locator('.comp-name').boundingBox();
 await page.mouse.move(title.x+25,title.y+5);await page.mouse.down();await page.mouse.move(title.x+40,title.y+5,{steps:3});
 assert.equal(await page.locator('.drag-ghost').count(),1,'fresh default-selected product must drag by its visible title, not open info');
 const lane=await page.locator('[data-component-drop="available"]').boundingBox();await page.mouse.move(lane.x+150,lane.y+120,{steps:12});await page.mouse.up();
 assert.equal(await page.evaluate(()=>window.__messages.length),initial+1);assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:0');
 assert.equal(await page.locator('#components-grid #product-settings32').count(),1);
 assert.equal(await page.locator('#components-installed-grid #product-settings32').count(),0);
 assert.equal(await page.locator('#product-settings64').isChecked(),true);
 assert.equal(await page.locator('#components-warning-dialog').isVisible(),false);
 // Replay real native facts after the intent, not just an unchanged local DOM.
 fresh.find(r=>r.id==='settings32').checked=false;
 await page.evaluate(components=>window.__receive({data:{type:'state',components}}),fresh);
 assert.equal(await page.locator('#components-grid #product-settings32').count(),1);
 await page.locator('[data-shelf="available"] [data-view="needed"]').click();
 assert.equal(await page.locator('#components-grid #product-settings32').isVisible(),true);
 await page.locator('#product-settings32').locator('..').locator('.comp-decision').click();
 assert.equal(await page.locator('#components-installed-grid #product-settings32').count(),1);
 await page.locator('#product-settings32').locator('..').locator('.comp-decision').click();
 assert.equal(await page.locator('#components-grid #product-settings32').count(),1);
 await page.locator('[data-shelf="available"] [data-view="all"]').click();
 // Cancel a title drag, delay past the old click-suppression timeout, then release.
 const x64=page.locator('#product-x64').locator('..');await x64.scrollIntoViewIfNeeded();
 const x64Name=await x64.locator('.comp-name').boundingBox(),beforeCancel=await page.evaluate(()=>window.__messages.length);
 await page.mouse.move(x64Name.x+20,x64Name.y+5);await page.mouse.down();await page.mouse.move(x64Name.x+35,x64Name.y+5);
 assert.equal(await page.locator('.drag-ghost').count(),1);await page.keyboard.press('Escape');await page.waitForTimeout(400);await page.mouse.up();
 assert.equal(await page.locator('#components-warning-dialog').isVisible(),false);assert.equal(await page.evaluate(()=>window.__messages.length),beforeCancel);
 await x64.locator('.comp-name').focus();await page.keyboard.press('Enter');
 assert.equal(await page.locator('#components-warning-dialog').isVisible(),true);await page.keyboard.press('Escape');
 console.log('PASS: fresh-install defaults: drag by product title back to Available, native echo, needed visibility, re-add/cancel via card controls, independent Settings, no spurious info popup.');
}
async function inactiveSutReturn(page,products){
 // Native export/inventory observations are tested separately in the C++ fakes.
 // This is their files-only UI result: inactive, no installed MSI, no conflict.
 const rows=products.map(r=>({...r,checked:false,installed:false,installedVersion:'',installedPackageVersion:'',operation:'install',disabled:false,locked:false,warning:'',registrationState:r.id==='secureUxTheme'?'inactive':undefined}));
 await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',appMode:'setup',maintenance:true,page:'components',components}}),rows);
 // The reported starting state: nothing installed or selected, only inert files.
 assert.equal(await page.locator('#components-installed-grid #product-secureUxTheme').count(),0);
 const absent=page.locator('#components-grid #product-secureUxTheme');
 assert.equal(await absent.isChecked(),false);
 assert.equal(await absent.locator('..').getAttribute('data-catalog-state'),'available');
 assert.equal(await absent.locator('..').evaluate(c=>c.classList.contains('active')),true);
 assert.equal(await absent.locator('..').locator('.comp-decision').isEnabled(),true);
 assert.equal(await page.locator('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').count(),0);
 const beforeInstall=await page.evaluate(()=>window.__messages.length);
 await absent.locator('..').scrollIntoViewIfNeeded();
 const from=await absent.locator('..').locator('.comp-name').boundingBox();
 const target=await page.locator('[data-component-drop="installed"]').boundingBox();
 await page.mouse.move(from.x+20,from.y+5);await page.mouse.down();await page.mouse.move(from.x+40,from.y+5,{steps:3});
 await page.mouse.move(target.x+150,target.y+100,{steps:12});await page.mouse.up();
 assert.equal(await page.evaluate(()=>window.__messages.length),beforeInstall+1);
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:1');
 assert.equal(await page.locator('#components-warning-dialog').isVisible(),false);
 assert.equal(await page.locator('#components-plan-items li:not(.plan-warning):not(.components-plan-empty)').count(),1);
 rows.find(r=>r.id==='secureUxTheme').checked=true;
 await page.evaluate(components=>window.__receive({data:{type:'state',components}}),rows);
 const initial=await page.evaluate(()=>window.__messages.length);
 const card=page.locator('#product-secureUxTheme').locator('..');await card.scrollIntoViewIfNeeded();
 const title=await card.locator('.comp-name').boundingBox();
 await page.mouse.move(title.x+20,title.y+5);await page.mouse.down();await page.mouse.move(title.x+40,title.y+5,{steps:3});
 assert.equal(await page.locator('.drag-ghost').count(),1);
 const lane=await page.locator('[data-component-drop="available"]').boundingBox();
 await page.mouse.move(lane.x+150,lane.y+120,{steps:12});await page.mouse.up();
 assert.equal(await page.evaluate(()=>window.__messages.length),initial+1);
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:0');
 assert.equal(await page.locator('#components-grid #product-secureUxTheme').count(),1);
 rows.find(r=>r.id==='secureUxTheme').checked=false;
 await page.evaluate(components=>window.__receive({data:{type:'state',components}}),rows);
 assert.equal(await page.locator('#components-grid #product-secureUxTheme').count(),1);
 console.log('PASS: files-only inactive SUT: initially Available/unchecked, active catalogue and empty plan; real title drag adds installation, then cancels it without removal warning; native echo preserves Available.');
}
async function registeredSutRemoval(page,products){
 const rows=products.map(r=>({...r,checked:false,installed:r.id==='secureUxTheme',installedVersion:r.id==='secureUxTheme'?'4.0.0':'',operation:'repair',warning:'',disabled:false,locked:false,removeSupported:r.id==='secureUxTheme',applySupported:r.id!=='secureUxTheme'}));
 await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',appMode:'setup',page:'components',components}}),rows);
 let initial=await page.evaluate(()=>window.__messages.length);
 let card=page.locator('#product-secureUxTheme').locator('..');await card.scrollIntoViewIfNeeded();
 assert.equal(await card.locator('.comp-remove').count(),1);
 assert.equal(await card.locator('.comp-decision').isDisabled(),true);
 const title=await card.locator('.comp-name').boundingBox();
 await page.mouse.move(title.x+20,title.y+5);await page.mouse.down();await page.mouse.move(title.x+40,title.y+5,{steps:3});
 assert.equal(await page.locator('.drag-ghost').count(),1);
 const lane=await page.locator('[data-component-drop="available"]').boundingBox();
 await page.mouse.move(lane.x+150,lane.y+120,{steps:12});await page.mouse.up();
 assert.equal(await page.evaluate(()=>window.__messages.length),initial+1);
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:remove');
 assert.equal(await card.evaluate(c=>c.classList.contains('pending-remove')),true);
 assert.equal(await page.locator('#components-plan-items li.remove').count(),1);
 // Concept C retains the committed card on Installed until Apply, marking removal.
 assert.equal(await page.locator('#components-installed-grid #product-secureUxTheme').count(),1);
 rows.find(r=>r.id==='secureUxTheme').checked=true;rows.find(r=>r.id==='secureUxTheme').operation='remove';
 await page.evaluate(components=>window.__receive({data:{type:'state',components}}),rows);
 card=page.locator('#product-secureUxTheme').locator('..');
 assert.equal(await card.locator('.comp-decision').getAttribute('title'),'Оставить установленным');
 await page.locator('#components-plan-items .plan-cancel').click();
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:0');
 assert.equal(await card.evaluate(c=>c.classList.contains('pending-remove')),false);
 await card.locator('.comp-remove').click();
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:remove');
 await card.locator('.comp-decision').click();
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:secureUxTheme:0');
 console.log('PASS: registered inactive SUT: actual removal capability, title drag emits explicit Remove IPC, C pending-removal/native echo, trash and keep/cancel; no fake repair.');
}
async function workspaceGestures(page,products,out){
 let epoch=0;
 const pin=async(name,on)=>{const b=page.locator('[data-component-pin="'+name+'"]');if(await b.getAttribute('aria-pressed')!==String(on))await b.click();};
 const seed=async(extra={},map=r=>r)=>{const rows=products.map(r=>({...map(r),fixtureEpoch:++epoch}));await page.evaluate(m=>window.__receive({data:m}),{type:'state',appMode:'setup',locale:'en',page:'components',componentsLoading:false,components:rows,...extra});return rows;};
 const count=()=>page.evaluate(()=>window.__messages.length);
 const begin=async(id,alias=false)=>{
  const card=alias?page.locator('[data-catalog-alias="'+id+'"]'):page.locator('#product-'+id).locator('..');await card.scrollIntoViewIfNeeded();
  const rect=await card.boundingBox(),meta=await card.locator('.c-meta').boundingBox();
  const x=meta.x+40,y=meta.y+4;
  await page.mouse.move(x,y);await page.mouse.down();await page.mouse.move(x+3,y);
  assert.equal(await page.locator('.drag-ghost').count(),0,'5px threshold');
  await page.mouse.move(x+12,y+2,{steps:3});return {rect,dx:x-rect.x,dy:y-rect.y};
 };
 const target=async name=>{const r=await page.locator('[data-component-drop="'+name+'"]').boundingBox();return {x:r.x+r.width/2,y:r.y+r.height-40};};
 const carry=async(id,alias=false)=>{const start=await begin(id,alias);assert.equal(await page.locator('.drag-ghost').count(),1);return start;};
 await pin('available',true);await pin('plan',true);
 await seed();const n=await count(),start=await carry('settings32'),to=await target('installed');
 await page.mouse.move(to.x,to.y,{steps:15});
 const ghost=await page.locator('.drag-ghost').boundingBox();
 assert.ok(Math.abs(ghost.x-(to.x-start.dx))<1&&Math.abs(ghost.y-(to.y-start.dy))<1,'ghost follows grip, not cursor centre');
 assert.ok(Math.abs(ghost.width-start.rect.width)<1);
 assert.equal(await page.locator('[data-component-drop="installed"]').evaluate(e=>e.classList.contains('drop-hover')),true);
 assert.equal(await page.locator('.drag-ghost input,.drag-ghost [id]').count(),0,'no duplicate intents or IDs');
 assert.equal(await page.locator('#components-panel .c-card input.comp-check').count(),6);
 if(out)await page.locator('#stage').screenshot({path:path.join(out,'production-card-drag.png')});
 // An unchanged host tick must not destroy the dragged catalogue row/ghost.
 await page.evaluate(()=>window.__receive({data:{type:'state',status:'unchanged host tick'}}));
 assert.equal(await page.locator('.drag-ghost').count(),1);assert.equal(await page.locator('.dragging').count(),1);
 await page.mouse.up();assert.equal(await count(),n+1);
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:1');
 assert.equal(await page.locator('#product-settings64').isChecked(),true);
 assert.equal(await page.locator('.drag-ghost,.dragging,.drop-hover').count(),0);
 // Backward transfer in Setup cancels an existing intent, never removes an MSI.
 await carry('settings32');const back=await target('available');await page.mouse.move(back.x,back.y,{steps:12});await page.mouse.up();
 assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:0');
 assert.equal(await page.locator('#product-settings32').isChecked(),false);
 // Catalogue aliases are draggable too, but still own no duplicate input/IPC.
 await seed();let aliasBefore=await count();await carry('x64',true);
 await page.evaluate(()=>window.__receive({data:{type:'state',status:'stable alias tick'}}));
 assert.equal(await page.locator('[data-catalog-alias="x64"].dragging').count(),1);
 const aliasTo=await target('installed');await page.mouse.move(aliasTo.x,aliasTo.y,{steps:10});await page.mouse.up();
 assert.equal(await count(),aliasBefore+1);assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:x64:1');
 assert.equal(await page.locator('#product-x64').locator('..').locator('.comp-decision.repair.is-on svg path').count(),4,'drop must retain the reference repair glyph, not a stale X refresh');
 // Escape, outside drop, blur, pointercancel and changed native facts never commit.
 for(const cancel of ['escape','outside','blur','pointercancel','changed','page','loading']){
  await seed();const before=await count();await carry('settings32');
  if(cancel==='escape')await page.keyboard.press('Escape');
  if(cancel==='outside')await page.mouse.move(900,75);
  if(cancel==='blur')await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
  if(cancel==='pointercancel')await page.dispatchEvent('#components-panel','pointercancel');
  if(cancel==='changed')await seed({},r=>({...r,disabled:r.id==='settings32'}));
  if(cancel==='loading')await page.evaluate(()=>window.__receive({data:{type:'state',componentsLoading:true}}));
  if(cancel==='page')await page.evaluate(()=>window.__receive({data:{type:'state',page:'checking'}}));
  await page.mouse.up();assert.equal(await count(),before,cancel+' must not send intent/cancel IPC');
  assert.equal(await page.locator('.drag-ghost,.dragging,.drop-hover').count(),0,cancel+' must clean up');
 }
 await seed({},r=>({...r,disabled:r.id==='settings32'}));let before=await count();await begin('settings32');
 assert.equal(await page.locator('.drag-ghost').count(),0);await page.mouse.up();assert.equal(await count(),before);
 // Keeping an installed Setup row cannot turn into an unimplemented Remove.
 await seed();before=await count();await carry('x64');const available=await target('available');await page.mouse.move(available.x,available.y,{steps:10});
 assert.equal(await page.locator('.drop-denied').count(),1);await page.mouse.up();assert.equal(await count(),before);
 // Direct Unins has an actual remove intent, unlike Setup's backward undo.
 await seed({appMode:'unins',page:'uninstall-select'},r=>({...r,installed:true,checked:false}));
 before=await count();await carry('x64');const removal=await target('available');await page.mouse.move(removal.x,removal.y,{steps:10});await page.mouse.up();
 assert.equal(await count(),before+1);assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:x64:1');
 // DPI/fit scaling must not double-scale the carried card.
 await seed();await page.setViewportSize({width:800,height:432});
 const scaled=await carry('settings32'),dest=await target('installed');await page.mouse.move(dest.x,dest.y,{steps:10});
 const scaledGhost=await page.locator('.drag-ghost').boundingBox();assert.ok(Math.abs(scaledGhost.width-scaled.rect.width)<1);assert.ok(Math.abs(scaledGhost.x-(dest.x-scaled.dx))<1);
 await page.keyboard.press('Escape');await page.mouse.up();await page.setViewportSize({width:1000,height:540});
 // Actual panel transitions, pinning, inert/focus protection, click and timed edge return.
 await seed();await page.emulateMedia({reducedMotion:'no-preference'});
 await pin('available',false);await pin('plan',true);
 const centre=page.locator('.c-lane.installed');await centre.click({position:{x:200,y:350}});
 await page.waitForFunction(()=>document.querySelector('.c-slot-left').classList.contains('is-hidden'));
 await page.waitForFunction(()=>Math.abs(document.querySelector('.c-lane.installed').getBoundingClientRect().width-610)<.2);
 assert.equal(await page.locator('.c-slot-left').evaluate(e=>e.inert),true);
 assert.equal(await page.locator('.c-slot-right').evaluate(e=>e.inert),false);
 assert.equal(await page.locator('.c-slot-left').evaluate(e=>getComputedStyle(e).transitionDuration),'0.34s');
 if(out)await page.locator('#stage').screenshot({path:path.join(out,'production-left-panel-hidden.png')});
 await seed({},r=>({...r,checked:r.checked||r.id==='settings32'}));const edgeBefore=await count();await carry('settings32');
 await page.mouse.move(16,270,{steps:10});await page.waitForTimeout(900);
 assert.equal(await page.locator('.c-slot-left').evaluate(e=>e.classList.contains('is-hidden')),true,'edge must not reopen immediately');
 await page.waitForFunction(()=>!document.querySelector('.c-slot-left').classList.contains('is-hidden'));
 assert.equal(await page.locator('.c-slot-right').evaluate(e=>e.classList.contains('is-hidden')),false,'pinned plan stays open');
 assert.equal(await page.locator('.drag-ghost').count(),1,'edge reveal keeps the drag alive');
 await page.waitForFunction(()=>Math.abs(document.querySelector('.c-slot-left').getBoundingClientRect().width-302)<.2);
 const edgeDrop=await target('available');await page.mouse.move(edgeDrop.x,edgeDrop.y,{steps:10});await page.mouse.up();
 assert.equal(await count(),edgeBefore+1);assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:0');
 // Re-hide; leaving an edge before 2s cancels its pending reveal.
 await centre.click({position:{x:200,y:350}});await page.waitForFunction(()=>document.querySelector('.c-slot-left').classList.contains('is-hidden'));
 await page.mouse.move(16,270);await page.waitForTimeout(400);await page.mouse.move(500,140);await page.waitForTimeout(1800);
 assert.equal(await page.locator('.c-slot-left').evaluate(e=>e.classList.contains('is-hidden')),true);
 await page.locator('[data-component-peek="available"]').click();await pin('available',true);await pin('plan',false);
 await centre.click({position:{x:180,y:350}});await page.waitForFunction(()=>document.querySelector('.c-slot-right').classList.contains('is-hidden'));
 await page.waitForFunction(()=>Math.abs(document.querySelector('.c-lane.installed').getBoundingClientRect().width-576)<.2);
 assert.equal(await page.locator('.c-slot-right').evaluate(e=>e.inert),true);
 await page.locator('[data-component-peek="plan"]').focus();await page.keyboard.press('Enter');
 assert.equal(await page.locator('.c-slot-right').evaluate(e=>e.inert),false);
 await pin('plan',true);await centre.click({position:{x:180,y:350}});await page.waitForTimeout(1250);
 assert.equal(await page.locator('.c-slot.is-hidden').count(),0,'both pinned panels stay visible');
 await seed();await page.emulateMedia({reducedMotion:'reduce'});
 console.log('PASS: real pointer ghost/grip/5px threshold, scaled drag, native tick, IPC/drop/undo, Escape/outside/blur/cancel/disabled/stale guards, direct Unins removal, panel animation/autohide/pins/2s edge/leave/click/keyboard/inert.');
}
(async()=>{
 const packaged=process.env.CHROMIUM_PATH?null:(require('@sparticuz/chromium').default||require('@sparticuz/chromium'));
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM_PATH||await packaged.executablePath(),args:packaged?packaged.args:[],headless:true,ignoreDefaultArgs:['--hide-scrollbars']});
 try {
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  const errors=[];page.on('pageerror',e=>{errors.push(e.message);console.error('Production/reference page error:',e.message);});
  await page.addInitScript(()=>{window.__messages=[];window.chrome={webview:{addEventListener(t,fn){if(t==='message')window.__receive=fn;},postMessage(m){window.__messages.push(m);}}};});
  await page.route('**/*',async route=>{
   const u=new URL(route.request().url());let file;
   if(u.pathname==='/')file=path.join(root,'GlassSetup/res/slideshow.html');
   else if(u.pathname==='/assets/banner.png')file=path.join(root,'GlassSetup/res/welcome_banner.png');
   else if(u.pathname.startsWith('/assets/'))file=path.join(root,'GlassSetup/res/assets',path.basename(u.pathname));
   else file=path.resolve(root,'.'+u.pathname);
   if(!file.startsWith(root+path.sep)||!fs.existsSync(file)||!fs.statSync(file).isFile())return route.fulfill({status:404,body:''});
   await route.fulfill({path:file});
  });
  await page.goto('https://glassplatform.invalid/');
  const products=[['x64','GlassPlatform x64'],['x86','GlassPlatform x86'],['settings64','GlassSettings x64'],['settings32','GlassSettings x86'],['openglass','OpenGlass'],['secureUxTheme','SecureUxTheme']].map(([id,title],i)=>({id,title,description:'Product information',installed:i<3,installedVersion:i<3?'1.0.0.0':'',targetVersion:'1.1.0.0',remoteVersion:'1.1.0.0',packageSize:16*1048576,checked:i===2,operation:i<3?'update':'install',warning:i===1?'Architecture-specific warning':'',state:i===2?'Обновить':'Без изменений',reasons:[]}));
  await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',page:'components',maintenance:true,components}}),products);
  await page.locator('#components-panel').waitFor({state:'visible'});
  await page.waitForFunction(()=>Math.abs(document.querySelector('.c-lane.available').getBoundingClientRect().y-document.querySelector('#stage').getBoundingClientRect().y-156)<.1);
  const read=()=>page.evaluate(()=>{
   const rect=s=>{const a=document.querySelector(s).getBoundingClientRect(),b=document.querySelector('#stage, .stage').getBoundingClientRect();return {x:Math.round(a.x-b.x),y:Math.round(a.y-b.y),w:Math.round(a.width),h:Math.round(a.height)};};
   return {available:rect('.c-lane.available'),installed:rect('.c-lane.installed'),plan:rect('.c-plan'),card:rect('.c-card')};
  });
  const skin=()=>page.evaluate(()=>{
   const card=document.createElement('article');card.className='comp-card c-card';document.querySelector('.c-lane-body').append(card);
   const c=getComputedStyle(card),lane=getComputedStyle(document.querySelector('.c-lane')),plan=getComputedStyle(document.querySelector('.c-plan'));
   const result={cardRadius:c.borderRadius,cardBackground:c.backgroundImage,cardShadow:c.boxShadow,laneRadius:lane.borderRadius,laneBackground:lane.backgroundImage,planRadius:plan.borderRadius,planBackground:plan.backgroundImage};card.remove();return result;
  });
  await page.locator('[data-component-pin="available"]').click();await page.locator('[data-component-pin="plan"]').click();
  const actual=await read(),actualSkin=await skin();
  assert.deepEqual(actual.available,{x:48,y:156,w:302,h:372});
  assert.deepEqual(actual.installed,{x:366,y:156,w:302,h:372});
  assert.deepEqual(actual.plan,{x:684,y:156,w:268,h:372});
  assert.equal(actual.card.h,72);
  await page.locator('#product-settings32').locator('..').locator('.comp-decision').click();
  assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:1');
  assert.equal(await page.locator('#components-installed-grid #product-settings32').count(),1);
  await page.locator('#product-settings32').locator('..').locator('.comp-name').click();
  await assert.equal(await page.locator('#components-warning-dialog').isVisible(),true);
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('#components-warning-dialog').isVisible(),false);
  const out=process.env.GLASS_UI_SCREENSHOT_DIR;
  await page.evaluate(()=>document.activeElement.blur());
  if(out){fs.mkdirSync(out,{recursive:true});await page.locator('#stage').screenshot({path:path.join(out,'production-maintenance.png')});}
  await neededFilterParity(page,products);
  await freshInstallReturn(page,products,out);
  await inactiveSutReturn(page,products);
  await registeredSutRemoval(page,products);
  await workspaceGestures(page,products,out);
  // Compare actual card content/control geometry with C, not only outer skins.
  await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',page:'components',appMode:'setup',components}}),[
   {id:'openglass',title:'OpenGlass',installed:false,checked:false,operation:'install',targetVersion:'1.0.0',remoteVersion:'1.0.0',packageSize:Math.round(12.8*1048576)}]);
  const productionAvailable=await cardContentSpec(page,'#components-grid [data-product="openglass"]');
  await page.locator('#product-openglass').locator('..').locator('.comp-decision').click();
  const productionPending=await cardContentSpec(page,'#components-installed-grid [data-product="openglass"]');
  const productionIdle=await cardContentSpec(page,'#components-grid [data-product="openglass"]');
  await page.setViewportSize({width:1180,height:800});
  await page.goto('https://glassplatform.invalid/GlassSetup/docs/mockups/product-maintenance-concepts.html');
  await page.evaluate(()=>document.querySelector('[data-pin="available"]').click());
  const reference=await read();assert.deepEqual(actualSkin,await skin(),'concept C card/lane/plan skins');
  assert.deepEqual(productionAvailable,await cardContentSpec(page,'.c-lane.available [data-id="openglass"].c-card'),'available content/action must match C');
  await page.locator('.c-lane.available [data-id="openglass"] [data-act="install"]').evaluate(button=>button.click());
  assert.deepEqual(productionPending,await cardContentSpec(page,'.c-lane.installed [data-id="openglass"].c-card'),'pending install content/action must match C');
  assert.deepEqual(productionIdle,await cardContentSpec(page,'.c-lane.available [data-id="openglass"].c-card'),'selected catalogue content/action must match C');
  for(const key of ['available','installed','plan'])assert.deepEqual(actual[key],reference[key],key+' must match approved concept C');
  assert.equal(actual.card.h,reference.card.h);
  if(out)await page.locator('.stage').screenshot({path:path.join(out,'reference-concept-c.png')});
  assert.deepEqual(errors,[]);
  console.log('PASS: real Chromium: exact C available/pending/idle card content, labels, SVG, control sizes and metadata alignment; shelf/plan rectangles and 72px cards, live Settings IPC/movement, product information and Escape. Not Windows/WebView runtime or a full pixel parity assertion.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
