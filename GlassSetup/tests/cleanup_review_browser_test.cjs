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
  let headingPosition;
  for(const locale of ['ru','en'])for(const [platform,og] of [[true,true],[true,false],[false,true]]){
   await page.evaluate(({locale,platform,og})=>{window.messages=[];window.receive({data:{type:'state',page:'full-cleanup',locale,cleanupBody:locale==='ru'?'Выберите, параметры каких продуктов нужно удалить.\nНеотмеченные параметры сохранятся для повторной установки.':'Choose which product settings to remove.\nUnchecked settings are kept for reinstallation.',cleanupPlatformEligible:platform,cleanupOgEligible:og,cleanupUserData:false,cleanupOpenGlassConfig:false}})}, {locale,platform,og});
   await page.waitForTimeout(400);
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
   for(const [eligible,id,key] of [[platform,'full','platform'],[og,'og','og']]){
    const label=page.locator('#'+id+'-cleanup-option');assert.equal(await label.isVisible(),eligible);
    if(!eligible)continue;
    const box=await label.boundingBox();assert.ok(box.y>=0&&box.y+box.height<=516,JSON.stringify(box));
    await label.click();assert.equal(await page.locator('#'+id+'-cleanup-choice').isChecked(),true);
    assert.ok((await page.evaluate(()=>window.messages)).includes('cleanup:'+key+':1'));
    assert.equal(await label.evaluate(e=>getComputedStyle(e).outlineStyle),'none','mouse click must not outline the whole card');
    await page.locator('#'+id+'-cleanup-choice').focus();

   }
  }
  console.log('PASS: real Chromium RU/EN cleanup labels clickable, independent IPC, all single/both-product states fit content bounds');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1)});
