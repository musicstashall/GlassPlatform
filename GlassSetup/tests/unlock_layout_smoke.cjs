// Production HTML in headless Chromium; no Windows operations or server.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright-core'),binaryModule=require('@sparticuz/chromium');
const binary=binaryModule.default||binaryModule;
(async()=>{
 const browser=await chromium.launch({executablePath:await binary.executablePath(),args:binary.args,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'no-preference'});
  await page.route('https://glassplatform.invalid/**',r=>r.abort());
  await page.addInitScript(()=>{window.chrome=window.chrome||{};window.chrome.webview={postMessage(){},addEventListener(type,fn){window.receiveHost=fn;}};});
  await page.goto('file://'+path.resolve(__dirname,'../res/slideshow.html'));
  const send=state=>page.evaluate(state=>window.receiveHost({data:{type:'state',...state}}),state);
  const state=()=>page.evaluate(()=>{
   const panel=document.getElementById('unlock-holders');
   const list=document.getElementById('unlock-holder-list'),report=panel.getBoundingClientRect();
   const copy=document.querySelector('#unlock-panel .unlock-copy').getBoundingClientRect();
   const filter=getComputedStyle(panel).backdropFilter||getComputedStyle(panel).webkitBackdropFilter||'';
   const animation=getComputedStyle(document.querySelector('.unlock-orbit')).animationName;
   return {hidden:panel.hidden,shade:!!document.getElementById('unlock-holder-shade'),filter,animation,
    inset:getComputedStyle(panel).left,left:report.left,right:report.right,top:report.top,bottom:report.bottom,copyRight:copy.right,scroll:list.scrollHeight>list.clientHeight,
    rows:list.children.length,title:document.getElementById('unlock-title').textContent,
     description:document.getElementById('unlock-description').textContent,status:document.getElementById('unlock-status').textContent};
  });
  for(const locale of ['ru','en'])for(const mode of ['unlock-modules','unlock-processes']){
   const holdReport=Array.from({length:18},(_,i)=>`process${i}.exe (86) (PID ${1000+i}): GlassCompat.dll, GlassCore.dll, WndCompat.dll`).join('\n');
   await send({locale,page:mode,unlockActive:false,holdReport});
   const waiting=await state();
   assert.equal(waiting.hidden,false);assert.equal(waiting.shade,false);assert.equal(waiting.animation,'none');
   assert.match(waiting.filter,/blur/i);
   assert.equal(waiting.inset,'480px');assert.ok(waiting.left>waiting.copyRight,JSON.stringify(waiting));
   assert.ok(waiting.top>=0&&waiting.bottom<=540&&waiting.right<=1000);assert.ok(waiting.scroll);assert.equal(waiting.rows,18);
   // Stale report remains in the host state during retry: never display it over activity.
   await send({unlockActive:true});
   const active=await state();assert.equal(active.hidden,true);assert.equal(active.shade,false);assert.notEqual(active.animation,'none');
   await send({unlockActive:false});
   assert.equal((await state()).hidden,false);
   await send({holdReport:''});assert.equal((await state()).hidden,true);
   await send({page:'welcome',holdReport});assert.equal((await state()).hidden,true);
  }
  for(const locale of ['ru','en']){
   const body=locale==='ru'?'Сохраните изменения в GlassSettings и закройте его окно. Затем нажмите «Повторить», чтобы продолжить установку.':'Save your changes in GlassSettings and close its window. Then select Retry to continue setup.';
   await send({locale,page:'unlock-processes',settingsClose:true,unlockActive:false,holdReport:'GlassSettings.exe (PID 4120): window still open',ui:{settingsCloseBody:body}});
   const settings=await state();
   assert.equal(settings.hidden,false);assert.equal(settings.shade,false);assert.match(settings.filter,/blur/i);
   assert.match(settings.status,/GlassSettings/);assert.equal(settings.description,body);
   assert.equal(settings.rows,1);assert.equal(settings.inset,'480px');
   await send({settingsClose:false});
   const processes=await state();
   assert.equal(processes.title,locale==='ru'?'Закрываем приложения':'Closing applications');
   assert.equal(settings.title,processes.title);assert.notEqual(processes.description,body);
   assert.match(processes.description,/принудительно|terminated/);
  }
  await page.emulateMedia({reducedMotion:'reduce'});
  await send({page:'unlock-modules',unlockActive:true});
  assert.equal((await state()).hidden,true);
  console.log('PASS: Chromium RU/EN holder plan panel: scroll, bounds, acrylic, idle/retry/residual/clear/navigation and reduced motion');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
