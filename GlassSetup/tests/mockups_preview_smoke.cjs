// Real Chromium + production HTML via the durable read-only mockup catalogue.
// NODE_PATH=<external packages> GLASS_PREVIEW_URL=<running preview> node this-file
// Dependencies: playwright-core and @sparticuz/chromium (no Windows operations).
const {chromium}=require('playwright-core');
const moduleBinary=require('@sparticuz/chromium');const binary=moduleBinary.default||moduleBinary;
const assert=require('node:assert/strict');
(async()=>{
 const base=process.env.GLASS_PREVIEW_URL;if(!base)throw Error('Set GLASS_PREVIEW_URL to the read-only preview server');
 const browser=await chromium.launch({executablePath:await binary.executablePath(),args:binary.args,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1200,height:920},reducedMotion:'reduce'}),errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  await page.goto(base);const ui=page.frameLocator('#preview');
  await ui.locator('#stage[data-mode="components"]').waitFor();
  assert.equal(await ui.locator('#components-installed-grid input.comp-check').count(),5);
  assert.equal(await ui.locator('#components-panel input.comp-check:checked').count(),0);
  await page.locator('#available').click();await page.locator('#plan').click();
  const update=ui.locator('[data-catalog-alias="settings64"]');
  await update.waitFor({state:'visible'});
  const green=await update.evaluate(card=>({tone:card.classList.contains('has-newer'),border:getComputedStyle(card).borderTopColor}));
  assert.ok(green.tone);assert.match(green.border,/131, 217, 166/);
  await update.hover();assert.match(await update.evaluate(card=>getComputedStyle(card).backgroundImage),/36, 71, 60/);
  const amber=ui.locator('#components-installed-grid [data-product="x64"]');
  await amber.hover();assert.match(await amber.evaluate(card=>getComputedStyle(card).backgroundImage),/148, 111, 45/);
  await amber.locator('.comp-decision').click();
  const activated=ui.locator('#components-installed-grid [data-product="x64"].is-activated');await activated.waitFor();
  await activated.hover();assert.match(await activated.evaluate(card=>getComputedStyle(card).backgroundImage),/36, 72, 60/);
  await activated.locator('.comp-decision').click();
  assert.doesNotMatch(await ui.locator('#components-plan-items').innerText(),/предупреждение/);
  const triangle=update.locator('.comp-warning-button');await triangle.click();
  await ui.locator('#components-warning-dialog').waitFor({state:'visible'});
  assert.match(await ui.locator('#components-warning-text').innerText(),/совместимость/);
  await page.keyboard.press('Escape');await ui.locator('#components-warning-dialog').waitFor({state:'hidden'});
  assert.equal(await triangle.evaluate(node=>node===document.activeElement),true);
  assert.equal(await ui.locator('#components-panel input.comp-check:checked').count(),0);
  await ui.locator('#components-remove-all').click();
  await page.waitForFunction(()=>document.getElementById('preview').contentDocument.querySelectorAll('#components-plan-items li.remove').length===5);
  assert.equal(await ui.locator('#components-installed-grid .c-card').count(),0);
  assert.equal(await ui.locator('#components-plan-items .plan-warning').count(),0);
  assert.ok(await ui.locator('#components-remove-all').isDisabled());
  assert.doesNotMatch(await page.locator('#messages').textContent(),/cleanup|install:start|uninstall:start/);
  await ui.locator('.plan-cancel').first().click();
  await ui.locator('#components-installed-grid input.comp-check').first().waitFor();
  await page.locator('#scenario').selectOption('damaged');
  await ui.locator('[data-catalog-alias="settings64"].is-broken').waitFor({state:'attached'});
  const damaged=ui.locator('[data-catalog-alias="settings64"]');
  assert.match(await damaged.evaluate(card=>getComputedStyle(card).borderTopColor),/229, 122, 122/);
  await page.locator('#available').click();await damaged.hover();assert.match(await damaged.evaluate(card=>getComputedStyle(card).backgroundImage),/148, 70, 83/);
  assert.equal(await ui.locator('#components-panel input.comp-check:checked').count(),0);
  await page.locator('#locale').selectOption('en');
  await ui.locator('#components-remove-all').filter({hasText:'Remove all'}).waitFor({state:'attached'});
  await page.locator('#app-mode').selectOption('unins');
  await ui.locator('#stage[data-mode="uninstall-select"]').waitFor();
  await page.locator('#plan').click();await ui.locator('#components-remove-all').click();
  await page.waitForFunction(()=>document.getElementById('preview').contentDocument.querySelectorAll('#components-plan-items li.remove').length===5);
  assert.match(await page.locator('#messages').textContent(),/components:select-all/);
  await page.locator('#scenario').selectOption('removed');await ui.locator('#stage[data-mode="complete"]').waitFor();
  assert.equal(await ui.locator('#complete-title').innerText(),'Removal complete');
  assert.equal(await ui.locator('#complete-panel.has-warnings').count(),0);
  await page.locator('#reference-tab').click();await page.locator('#reference').waitFor({state:'visible'});
  assert.ok(await page.locator('#reference img').evaluate(img=>img.complete&&img.naturalWidth>0));
  await page.locator('#runtime-tab').click();await page.locator('#locale').selectOption('ru');
  await page.locator('#app-mode').selectOption('setup');await page.locator('#scenario').selectOption('updates');
  await ui.locator('#stage[data-mode="components"]').waitFor();
  await page.locator('#available').click();await page.locator('#plan').click();
  if(process.env.GLASS_PREVIEW_SCREENSHOT)await page.screenshot({path:process.env.GLASS_PREVIEW_SCREENSHOT,fullPage:true});
  // Empty borders fill each usable list plane, not a 120px card at the top.
  await page.locator('#scenario').selectOption('empty');
  await ui.locator('#components-grid.is-empty .c-drop').waitFor({state:'attached'});
  await page.locator('#available').click();await page.locator('#plan').click();
  for(const id of ['components-grid','components-installed-grid']){
   const geometry=await ui.locator('#'+id).evaluate(grid=>{
    const body=grid.closest('.c-lane-body'),box=body.getBoundingClientRect(),empty=grid.querySelector('.c-drop'),rect=empty.getBoundingClientRect(),style=getComputedStyle(body),content=getComputedStyle(empty);
    return {left:rect.left-box.left,right:box.right-rect.right,top:rect.top-box.top,bottom:box.bottom-rect.bottom,
     px:parseFloat(style.paddingLeft),py:parseFloat(style.paddingTop),mask:style.maskImage,align:content.alignItems,justify:content.justifyContent};
   });
   assert.ok(Math.abs(geometry.left-geometry.px)<1);assert.ok(Math.abs(geometry.right-geometry.px)<1);
   assert.ok(Math.abs(geometry.top-geometry.py)<1);assert.ok(Math.abs(geometry.bottom-geometry.py)<1);
   assert.equal(geometry.mask,'none');assert.equal(geometry.align,'center');assert.equal(geometry.justify,'center');
  }
  const planGeometry=await ui.locator('.components-plan-empty').evaluate(empty=>{
   const a=empty.getBoundingClientRect(),b=empty.parentElement.getBoundingClientRect();return {gap:b.height-a.height,top:a.top-b.top,width:b.width-a.width,justify:getComputedStyle(empty).justifyContent};
  });
  assert.ok(Math.abs(planGeometry.gap)<1&&Math.abs(planGeometry.top)<1&&Math.abs(planGeometry.width)<1);assert.equal(planGeometry.justify,'center');
  if(process.env.GLASS_PREVIEW_EMPTY_SCREENSHOT)await page.screenshot({path:process.env.GLASS_PREVIEW_EMPTY_SCREENSHOT,fullPage:true});
  await page.locator('#scenario').selectOption('checking');await ui.locator('#stage[data-mode="checking"]').waitFor();
  assert.equal(await ui.locator('#checking-copy').textContent(),'Ищем обновление GlassSetup.');
  assert.doesNotMatch(await ui.locator('#checking-copy').textContent(),/скач|продукт|другой странице/);
  await page.locator('#locale').selectOption('en');await ui.locator('#checking-copy').filter({hasText:'Looking for a GlassSetup update.'}).waitFor();
  await page.locator('#locale').selectOption('ru');await page.locator('#scenario').selectOption('sut-local');
  await ui.locator('#product-secureUxTheme').waitFor({state:'attached'});await page.locator('#available').click();
  const sut=ui.locator('#product-secureUxTheme').locator('..');
  assert.equal(await sut.locator('.comp-version-line').textContent(),'4.0.0 · 0,5 МБ');
  assert.match(await sut.locator('.c-src').textContent(),/пакет 4.0.0/);assert.match(await sut.locator('.c-src').textContent(),/релиз 4.1.0/);
  await page.locator('#scenario').selectOption('sut-release');await ui.locator('#components-panel .comp-version-line').filter({hasText:'релиз 4.1.0'}).waitFor({state:'attached'});await page.locator('#available').click();
  assert.match(await sut.locator('.comp-version-line').textContent(),/релиз 4.1.0/);assert.doesNotMatch(await sut.locator('.c-src').textContent(),/пакет/);
  await sut.locator('.comp-decision').click();await ui.locator('#components-installed-grid #product-secureUxTheme').waitFor({state:'attached'});
  assert.match(await sut.locator('.comp-version-line').textContent(),/релиз 4.1.0/);
  // Product details and long warnings must reserve footer space, not inherit
  // the slideshow's absolutely positioned footer (the reported Windows overlap).
  await sut.locator('.comp-name').click();
  const dialog=ui.locator('#components-warning-dialog');await dialog.waitFor({state:'visible'});
  async function assertDialogLayout(scrolls){
   const g=await dialog.evaluate(shade=>{
    const section=shade.querySelector('section'),body=shade.querySelector('#components-warning-text'),footer=shade.querySelector('footer'),ok=shade.querySelector('#components-warning-ok');
    const b=body.getBoundingClientRect(),f=footer.getBoundingClientRect(),s=section.getBoundingClientRect(),k=ok.getBoundingClientRect();
    return {bodyBottom:b.bottom,footerTop:f.top,footerBottom:f.bottom,sectionBottom:s.bottom,buttonBottom:k.bottom,position:getComputedStyle(footer).position,scrolls:body.scrollHeight>body.clientHeight+1};
   });
   assert.equal(g.position,'static');assert.ok(g.bodyBottom<=g.footerTop+1,JSON.stringify(g));
   assert.ok(g.footerBottom<=g.sectionBottom+1,JSON.stringify(g));assert.ok(g.buttonBottom<g.sectionBottom,JSON.stringify(g));
   if(scrolls)assert.ok(g.scrolls,JSON.stringify(g));
  }
  await assertDialogLayout(false);await ui.locator('#components-warning-ok').click();
  for(const language of ['ru','en']){
   await ui.locator('#stage').evaluate((stage,language)=>{
    setLocale(language);
    showProductWarning({id:'secureUxTheme',title:'SecureUxTheme',information:true,description:'Product description. '.repeat(30),state:'Restart required',targetVersion:'4.0.0',presenceDetail:'Installed MSI inventory rc=0; {GUID} state=5',reasons:Array(15).fill('Long readable product note. '.repeat(8))},document.querySelector('#product-secureUxTheme').closest('.c-card').querySelector('.comp-name'));
   },language);
   await assertDialogLayout(true);
   assert.doesNotMatch(await ui.locator('#components-warning-text').innerText(),/Installed MSI inventory|GUID/);
   await ui.locator('#components-warning-text').evaluate(n=>{n.scrollTop=n.scrollHeight;});await assertDialogLayout(true);
   await ui.locator('#components-warning-ok').click();
   await ui.locator('#stage').evaluate(()=>showProductWarning({title:'Dependencies affected',planWarnings:Array(20).fill('Long warning. '.repeat(20))},document.getElementById('components-select-all')));
   await assertDialogLayout(true);await page.keyboard.press('Escape');
  }
  // Blocked SUT deletion explains the real prerequisite without posting Remove.
  for(const scenario of ['sut-active','sut-pending']){
   await page.locator('#scenario').selectOption(scenario);
   const info=ui.locator('#components-installed-grid .comp-remove.removal-info');await info.waitFor();
   assert.equal(await ui.locator('#components-remove-all').isVisible(),false);
   await info.focus();await page.keyboard.press('Enter');await dialog.waitFor({state:'visible'});
   assert.match(await ui.locator('#components-warning-text').innerText(),scenario==='sut-active'?/не реализовано/:/перезагрузки/);
   assert.equal(await ui.locator('#product-secureUxTheme').isChecked(),false);
   assert.doesNotMatch(await page.locator('#messages').textContent(),/component:secureUxTheme:remove/);
   await page.keyboard.press('Escape');assert.equal(await info.evaluate(n=>n===document.activeElement),true);
  }
  const concept=await browser.newPage();concept.on('pageerror',error=>errors.push(error.message));
  await concept.goto(new URL('product-maintenance-concepts.html',base).href);
  assert.ok(await concept.locator('.stage-banner').first().evaluate(img=>img.complete&&img.naturalWidth>0));
  await concept.close();
  assert.deepEqual(errors,[]);
  console.log('PASS: Chromium mockup: production UI, green idle/hover updates, red damage, triangle popup/Escape/focus, bulk plan/undo, Setup/Unins, RU/EN, completion, supplied reference');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
