// Проверяет выделение, Ctrl+C и единственную команду контекстного меню в production HTML.
const {chromium}=require('playwright-core');
const assert=require('node:assert/strict');
const path=require('node:path');
const {pathToFileURL}=require('node:url');

(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM_PATH});
 try{
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  await page.addInitScript(()=>{
   window.messages=[];
   window.chrome={webview:{postMessage:m=>window.messages.push(m),addEventListener:(_type,f)=>{window.receive=f;}}};
   window.copies=[];
   document.addEventListener('copy',()=>window.copies.push(String(window.getSelection())));
  });
  await page.goto(pathToFileURL(path.resolve(__dirname,'../res/slideshow.html')).href);
  const detail=page.locator('#error-detail'),menu=page.getByRole('menu'),command=page.getByRole('menuitem');
  for(const [locale,appMode,caption] of [['ru','setup','Копировать'],['en','unins','Copy']]){
   const resourceCaption=`${caption} · ${appMode}`;
   await page.evaluate(({locale,appMode,resourceCaption})=>{
    window.receive({data:{type:'init',locale,appMode,ui:{errorCopy:resourceCaption}}});
    window.receive({data:{type:'state',page:'error',errorReason:'Test error detail\nSecond line'}});
   },{locale,appMode,resourceCaption});
   await detail.waitFor({state:'visible'});
   assert.equal(await detail.evaluate(e=>getComputedStyle(e).userSelect),'text');
   await detail.evaluate(e=>{
    e.focus();
    const selection=window.getSelection(),range=document.createRange();
    range.setStart(e.firstChild,0);range.setEnd(e.firstChild,10);
    selection.removeAllRanges();selection.addRange(range);
   });
   // Повторная синхронизация не должна сбрасывать выделение.
   await page.evaluate(()=>window.receive({data:{type:'state',page:'error',status:'Updated'}}));
   assert.equal(await page.evaluate(()=>String(window.getSelection())),'Test error');
   await page.keyboard.press('Control+c');
   await detail.click({button:'right',position:{x:10,y:6}});
   await menu.waitFor({state:'visible'});
   assert.equal(await command.count(),1);
   assert.equal(await command.textContent(),resourceCaption);
   await command.click();
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),['Test error','Test error']);
   assert.equal(await menu.isVisible(),false);
   await page.evaluate(()=>window.receive({data:{type:'state',page:'error',errorReason:''}}));
   assert.equal(await detail.isVisible(),false);
  }
  await page.evaluate(()=>window.receive({data:{type:'state',page:'complete',errorReason:'Hidden detail'}}));
  assert.equal(await detail.isVisible(),false);
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
