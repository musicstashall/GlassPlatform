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
   const panel=document.getElementById('unlock-holders'),shade=document.getElementById('unlock-holder-shade');
   const list=document.getElementById('unlock-holder-list'),report=panel.getBoundingClientRect();
   const copy=document.querySelector('#unlock-panel .unlock-copy').getBoundingClientRect();
   return {hidden:panel.hidden,shadeHidden:shade.hidden,animation:getComputedStyle(document.querySelector('.unlock-orbit')).animationName,
    left:report.left,right:report.right,top:report.top,bottom:report.bottom,copyRight:copy.right,scroll:list.scrollHeight>list.clientHeight,
    shadeColor:getComputedStyle(shade).backgroundColor,rows:list.children.length};
  });
  for(const locale of ['ru','en'])for(const mode of ['unlock-modules','unlock-processes']){
   const holdReport=Array.from({length:18},(_,i)=>`process${i}.exe (86) (PID ${1000+i}): GlassCompat.dll, GlassCore.dll, WndCompat.dll`).join('\n');
   await send({locale,page:mode,unlockActive:false,holdReport});
   const waiting=await state();
   assert.equal(waiting.hidden,false);assert.equal(waiting.shadeHidden,false);assert.equal(waiting.animation,'none');
   assert.ok(waiting.left>waiting.copyRight,JSON.stringify(waiting));
   assert.ok(waiting.top>=0&&waiting.bottom<=540&&waiting.right<=1000);assert.ok(waiting.scroll);assert.equal(waiting.rows,18);
   assert.notEqual(waiting.shadeColor,'rgba(0, 0, 0, 0)');
   // Stale report remains in the host state during retry: never display it over activity.
   await send({unlockActive:true});
   const active=await state();assert.equal(active.hidden,true);assert.equal(active.shadeHidden,true);assert.notEqual(active.animation,'none');
   await send({unlockActive:false});
   assert.equal((await state()).hidden,false);assert.equal((await state()).shadeHidden,false);
   await send({holdReport:''});assert.equal((await state()).hidden,true);assert.equal((await state()).shadeHidden,true);
   await send({page:'welcome',holdReport});assert.equal((await state()).hidden,true);assert.equal((await state()).shadeHidden,true);
  }
  await page.emulateMedia({reducedMotion:'reduce'});
  await send({page:'unlock-modules',unlockActive:true});
  assert.equal((await state()).hidden,true);
  console.log('PASS: Chromium RU/EN holder plan panel: scroll, bounds, shade, idle/retry/residual/clear/navigation and reduced motion');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
