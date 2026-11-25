// Real Chromium hit testing and geometry of the production cleanup page.
const {chromium}=require('playwright-core'),pkg=require('@sparticuz/chromium');
const binary=pkg.default||pkg,path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({executablePath:await binary.executablePath(),args:binary.args,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  await page.addInitScript(()=>{window.messages=[];window.chrome={webview:{postMessage(m){window.messages.push(m)},addEventListener(t,f){window.receive=f;}}};});
  await page.goto('file://'+path.resolve(__dirname,'../res/slideshow.html'));
  await page.evaluate(()=>window.receive({data:{type:'state',page:'certificate',locale:'ru'}}));
  await page.waitForTimeout(400);
  const reference=await page.locator('#certificate-title').boundingBox();
  for(const locale of ['ru','en'])for(const [mode,id] of [['components','components-title'],['license','license-title'],['certificate','certificate-title'],['confirm-cancel','confirm-title'],['checking','checking-title'],['conflicts','conflicts-title'],['error','error-title'],['complete','complete-title'],['cancelled','cancelled-title'],['launching','launch-title'],['uninstalling','unins-title'],['cancelling','cancelling-title'],['unlock-modules','unlock-title']]){
   await page.evaluate(({mode,locale})=>window.receive({data:{type:'state',page:mode,locale,components:[],componentsLoading:false}}),{mode,locale});
   await page.waitForTimeout(950);
   const heading=await page.locator('#'+id).boundingBox();
   assert.ok(Math.abs(heading.y-reference.y)<2,JSON.stringify({mode,locale,heading,reference}));
   assert.equal(heading.x,reference.x);
  }
  for(const locale of ['ru','en']){
   await page.evaluate(locale=>window.receive({data:{type:'state',page:'certificate',locale,
    certificate:{subject:'CN=Example Publisher',thumbprint:'0123456789ABCDEF0123456789ABCDEF01234567',validity:'2026-01-01 – 2027-01-01'},
    ui:{certSubjectLabel:'Subject',certThumbprintLabel:'Thumbprint',certValidityLabel:'Validity',certStoreLabel:'Store',certDetailsLabel:'Certificate'}}}),locale);
   const facts=await page.locator('.cert-facts').boundingBox(),last=await page.locator('#certificate-options .cert-card').last().boundingBox();
   assert.ok(Math.abs(facts.y+facts.height-last.y-last.height)<1,JSON.stringify({locale,facts,last}));
   assert.ok(facts.y+facts.height<540);
   const overlay=await page.locator('#certificate-panel .components-overlay').evaluate(e=>getComputedStyle(e).backgroundImage);
   const opacities=[...overlay.matchAll(/rgba\([^)]*,\s*([\d.]+)\)/g)].map(m=>Number(m[1]));
   assert.equal(opacities.length,2);assert.ok(opacities.every(alpha=>alpha>=.4));
  }
  const cardSkin=e=>{const s=getComputedStyle(e);return {padding:s.padding,borderRadius:s.borderRadius,background:s.backgroundImage,shadow:s.boxShadow};};
  const certificateSkin=await page.locator('#certificate-options .cert-card:not(.is-checked)').evaluate(cardSkin);
  let headingPosition;
  for(const locale of ['ru','en'])for(const [platform,og] of [[true,true],[true,false],[false,true]]){
   await page.evaluate(({locale,platform,og})=>{window.messages=[];window.receive({data:{type:'state',page:'full-cleanup',locale,cleanupBody:locale==='ru'?'Выберите, параметры каких продуктов нужно удалить.\nНеотмеченные параметры сохранятся для повторной установки.':'Choose which product settings to remove.\nUnchecked settings are kept for reinstallation.',cleanupPlatformEligible:platform,cleanupOgEligible:og,cleanupUserData:false,cleanupOpenGlassConfig:false}})}, {locale,platform,og});
   await page.waitForTimeout(400);
   const background=page.locator('#confirm-banner');
   assert.equal(await background.isVisible(),true,'cleanup must show the shared maintenance banner');
   assert.equal(await background.getAttribute('src'),await page.locator('#components-banner').getAttribute('src'));
   assert.equal(await background.evaluate(e=>getComputedStyle(e).opacity),'0.34');
   const backup=page.locator('#cleanup-backup'),backupBox=await backup.boundingBox();
   assert.ok(backupBox&&backupBox.x>=550&&backupBox.x+backupBox.width<=953&&backupBox.y+backupBox.height<=500,JSON.stringify(backupBox));
   assert.match(await backup.innerText(),/GlassSettings/);
   assert.equal(await backup.locator('h2 svg').count(),1);
   const paragraphs=await backup.locator('p').evaluateAll(nodes=>nodes.map(e=>({font:getComputedStyle(e).fontSize,line:getComputedStyle(e).lineHeight,bullet:getComputedStyle(e,':before').content})));
   assert.equal(paragraphs[0].font,paragraphs[1].font);assert.equal(paragraphs[0].line,paragraphs[1].line);
   assert.ok(paragraphs.every(p=>p.bullet.includes('•')));
   assert.match(await backup.innerText(),locale==='ru'?/страницу «Системы»/:/System page/);
   await page.mouse.move(0,0);
   for(const card of await page.locator('.cleanup-option:not([hidden])').all()){
    const rect=await card.boundingBox();assert.ok(rect.width<=480&&rect.x+rect.width<backupBox.x&&rect.height<78);
    assert.deepEqual(await card.evaluate(cardSkin),certificateSkin);
   }
   const choices=await page.locator('.cleanup-option:not([hidden]) small').allTextContents();
   assert.equal(choices.length,Number(platform)+Number(og));
   for(const text of choices)assert.ok(!/uninstaller|profiles|штатн|профил|файл|темы/i.test(text));
   const body=await page.locator('#confirm-body').innerText(),note=await page.locator('#confirm-note').innerText();
   if(platform!==og){assert.ok(body.includes(og?'OpenGlass':'GlassPlatform'));assert.ok(!note.includes('каждого')&&!note.includes('each product'));}
   const bounds=await page.locator('#confirm-tag').boundingBox();
   const end=await page.locator('#confirm-note').boundingBox();
   const title=await page.locator('#confirm-title').boundingBox();
   assert.equal(title.x,reference.x,'cleanup and certificate share the left heading grid');
   assert.ok(Math.abs(title.y-reference.y)<2,JSON.stringify({title,reference}));
   if(headingPosition)assert.deepEqual({x:title.x,y:title.y},headingPosition,'heading must not move with product count or locale');
   headingPosition={x:title.x,y:title.y};
   assert.ok(bounds.y>=76&&bounds.y<=80&&end.y+end.height<510);
   assert.ok(end.y>440,'preservation hints sit at the bottom, not beneath the last choice');
   assert.equal(await page.locator('#cleanup-explanation').isVisible(),false);
   assert.ok(end.height<36,'one-line preservation hint including 16px top spacing');
   assert.equal(await page.locator('#confirm-note').evaluate(e=>{const r=document.createRange();r.selectNodeContents(e);return r.getClientRects().length}),1);
   for(const [eligible,id,key] of [[platform,'full','platform'],[og,'og','og']]){
    const label=page.locator('#'+id+'-cleanup-option');assert.equal(await label.isVisible(),eligible);
    if(!eligible)continue;
    const box=await label.boundingBox();assert.ok(box.y>=0&&box.y+box.height<=516,JSON.stringify(box));
    const choice=page.locator('#'+id+'-cleanup-choice');
    await label.click();assert.equal(await choice.isChecked(),true);
    assert.equal(await choice.getAttribute('type'),'checkbox');
    const marker=await choice.evaluate(e=>{const mark=getComputedStyle(e,'::after');return {radius:getComputedStyle(e).borderRadius,left:mark.borderLeftWidth,bottom:mark.borderBottomWidth};});
    assert.deepEqual(marker,{radius:'5px',left:'2px',bottom:'2px'});
    assert.ok((await page.evaluate(()=>window.messages)).includes('cleanup:'+key+':1'));
    assert.equal(await label.evaluate(e=>getComputedStyle(e).outlineStyle),'none','mouse click must not outline the whole card');
    await choice.focus();
   }
   if(platform&&og){
    const platformChoice=page.locator('#full-cleanup-choice'),ogChoice=page.locator('#og-cleanup-choice');
    assert.equal(await platformChoice.isChecked(),true);assert.equal(await ogChoice.isChecked(),true);
    await platformChoice.press('Space');
    assert.equal(await platformChoice.isChecked(),false);assert.equal(await ogChoice.isChecked(),true);
    assert.ok((await page.evaluate(()=>window.messages)).includes('cleanup:platform:0'));
   }
  }
  for(const locale of ['ru','en']){
   await page.evaluate(locale=>window.receive({data:{type:'state',page:'complete',locale,ui:{},finishWarnings:[]}}),locale);
   assert.equal(await page.locator('#complete-ready-text').innerText(),locale==='ru'?'ОПЕРАЦИИ ВЫПОЛНЕНЫ':'OPERATIONS COMPLETED');
  }
  for(const detail of ['', '   \n  ', 'Hosts restored']){
   await page.evaluate(detail=>window.receive({data:{type:'state',page:'cancelled',cancellationIsRemoval:true,cancelledDetail:detail,ui:{cancelledDetail:'stale fallback'}}}),detail);
   await page.waitForTimeout(400);
   const note=page.locator('#cancelled-detail');
   assert.equal(await note.isVisible(),!!detail.trim());
   if(!detail.trim())assert.equal(await note.boundingBox(),null);
   else {
    const style=await note.evaluate(e=>({border:getComputedStyle(e).borderTopWidth,background:getComputedStyle(e).backgroundColor,padding:getComputedStyle(e).padding}));
    assert.equal(style.border,'0px');assert.equal(style.padding,'0px');assert.equal(style.background,'rgba(0, 0, 0, 0)');
   }
  }
  console.log('PASS: real Chromium RU/EN cleanup labels clickable, independent IPC, all single/both-product states fit content bounds');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
