// Проверяет выделение мышью, Ctrl+A/Ctrl+C и контекстное меню финальных страниц.
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
  const selection=()=>page.evaluate(()=>String(window.getSelection()));
  async function dragText(locator,start,end){
   const points=await locator.evaluate((e,{start,end})=>[start,end].map(offset=>{
    const range=document.createRange();
    range.setStart(e.firstChild,offset);range.collapse(true);
    const {x,y,height}=range.getBoundingClientRect();return {x,y:y+height/2};
   }),{start,end});
   await page.mouse.move(points[0].x,points[0].y);
   await page.mouse.down();
   await page.mouse.move(points[1].x,points[1].y,{steps:8});
   await page.mouse.up();
   return {x:(points[0].x+points[1].x)/2,y:points[0].y};
  }
  for(const [locale,appMode,caption] of [['ru','setup','Копировать'],['en','unins','Copy']]){
   const resourceCaption=`${caption} · ${appMode}`;
   await page.evaluate(({locale,appMode,resourceCaption})=>{
    window.receive({data:{type:'init',locale,appMode,ui:{errorCopy:resourceCaption}}});
    window.receive({data:{type:'state',page:'error',errorReason:'Test error detail\nSecond line'}});
   },{locale,appMode,resourceCaption});
   await detail.waitFor({state:'visible'});
   assert.equal(await detail.evaluate(e=>getComputedStyle(e).userSelect),'text');
   const errorPoint=await dragText(detail,0,10);
   assert.equal(await detail.evaluate(e=>e===document.activeElement),true);
   // Повторная синхронизация не должна сбрасывать выделение.
   await page.evaluate(()=>window.receive({data:{type:'state',page:'error',status:'Updated'}}));
   assert.equal(await page.evaluate(()=>String(window.getSelection())),'Test error');
   await page.keyboard.press('Control+c');
   await page.mouse.click(errorPoint.x,errorPoint.y,{button:'right'});
   await menu.waitFor({state:'visible'});
   assert.equal(await command.count(),1);
   assert.equal(await command.textContent(),resourceCaption);
   await command.click();
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),['Test error','Test error']);
   assert.equal(await menu.isVisible(),false);
   await page.mouse.click(errorPoint.x,errorPoint.y);
   await page.keyboard.press('Control+a');
   assert.equal(await selection(),'Test error detail\nSecond line');
   await page.keyboard.press('Control+c');
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),['Test error detail\nSecond line']);
   await page.mouse.click(errorPoint.x,errorPoint.y);
   await page.mouse.click(errorPoint.x,errorPoint.y,{button:'right'});
   await menu.waitFor({state:'visible'});
   assert.equal(await command.getAttribute('aria-disabled'),'true');
   const box=await command.boundingBox();
   await page.mouse.click(box.x+box.width/2,box.y+box.height/2);
   assert.equal(await menu.isVisible(),false);
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[]);
   await page.evaluate(()=>window.receive({data:{type:'state',page:'error',errorReason:''}}));
   assert.equal(await detail.isVisible(),false);

   const finishWarnings=Array.from({length:12},(_,i)=>({title:`Warning ${i+1}`,detail:`Follow-up detail ${i+1}`}));
   const sendWarnings=finishWarnings=>page.evaluate(finishWarnings=>window.receive({data:{type:'state',page:'complete',finishWarnings}}),finishWarnings);
   await sendWarnings(finishWarnings);
   await warnings.waitFor({state:'visible'});
   assert.ok(await warnings.evaluate(e=>{
    e.scrollTop=2;
    return e.scrollHeight>e.clientHeight&&e.scrollTop>0;
   }));
   const warningPoint=await dragText(warnings.locator('li span').first(),0,9);
   assert.equal(await warnings.evaluate(e=>e===document.activeElement),true);
   const selected=await selection();
   assert.equal(selected,'Follow-up');
   await sendWarnings(finishWarnings);
   assert.equal(await page.evaluate(()=>String(window.getSelection())),selected);
   assert.ok(await warnings.evaluate(e=>e.scrollTop>0));
   await page.keyboard.press('Control+c');
   await page.keyboard.press('Shift+F10');
   await menu.waitFor({state:'visible'});
   assert.equal(await command.textContent(),resourceCaption);
   await page.keyboard.press('Enter');
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[selected,selected]);
   await page.mouse.click(warningPoint.x,warningPoint.y,{button:'right'});
   await command.click();
   assert.equal(await menu.isVisible(),false);
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[selected]);
   await page.mouse.click(warningPoint.x,warningPoint.y);
   await page.keyboard.press('Control+a');
   const allWarnings=await selection();
   assert.match(allWarnings,/^Warning 1\s+Follow-up detail 1\b/);
   assert.match(allWarnings,/Warning 12\s+Follow-up detail 12\s*$/);
   await page.keyboard.press('Control+c');
   assert.deepEqual(await page.evaluate(()=>window.copies.splice(0)),[allWarnings]);
   await page.mouse.click(warningPoint.x,warningPoint.y,{button:'right'});
   await menu.waitFor({state:'visible'});
   await sendWarnings([]);
   assert.equal(await warnings.isVisible(),false);
   assert.equal(await menu.isVisible(),false);
  }
  await page.evaluate(()=>window.receive({data:{type:'state',page:'complete',errorReason:'Hidden detail'}}));
  assert.equal(await detail.isVisible(),false);
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
