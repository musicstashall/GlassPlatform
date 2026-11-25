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
  const detail=page.locator('#error-detail'),warnings=page.locator('#complete-warn-list'),menu=page.getByRole('menu'),command=page.getByRole('menuitem');
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

   const finishWarnings=Array.from({length:12},(_,i)=>({title:`Warning ${i+1}`,detail:`Follow-up detail ${i+1}`}));
   const sendWarnings=finishWarnings=>page.evaluate(finishWarnings=>window.receive({data:{type:'state',page:'complete',finishWarnings}}),finishWarnings);
   await sendWarnings(finishWarnings);
   await warnings.waitFor({state:'visible'});
   const canScroll=await warnings.evaluate(e=>{
    e.focus();e.scrollTop=2;
    const selection=window.getSelection(),range=document.createRange();
    range.selectNodeContents(e.firstElementChild);
    selection.removeAllRanges();selection.addRange(range);
    return e.scrollHeight>e.clientHeight&&e.scrollTop>0&&getComputedStyle(e).userSelect==='text';
   });
   assert.ok(canScroll);
   const selected=await page.evaluate(()=>String(window.getSelection()));
   assert.match(selected,/Warning 1\s+Follow-up detail 1/);
   await sendWarnings(finishWarnings);
   assert.equal(await page.evaluate(()=>String(window.getSelection())),selected);
   assert.ok(await warnings.evaluate(e=>e.scrollTop>0));
   await page.keyboard.press('Control+c');
   await page.keyboard.press('Shift+F10');
   await menu.waitFor({state:'visible'});
   assert.equal(await command.textContent(),resourceCaption);
   await page.keyboard.press('Enter');
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[selected,selected]);
   await warnings.click({button:'right',position:{x:25,y:6}});
   await command.click();
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[selected]);
   await warnings.click({button:'right',position:{x:25,y:6}});
   await menu.waitFor({state:'visible'});
   await sendWarnings([]);
   assert.equal(await warnings.isVisible(),false);
   assert.equal(await menu.isVisible(),false);
  }
  await page.evaluate(()=>window.receive({data:{type:'state',page:'complete',errorReason:'Hidden detail'}}));
  assert.equal(await detail.isVisible(),false);
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
