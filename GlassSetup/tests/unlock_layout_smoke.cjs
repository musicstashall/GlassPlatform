// Production HTML in headless Chromium; no Windows operations or server.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright-core'),binaryModule=require('@sparticuz/chromium');
const binary=binaryModule.default||binaryModule;
(async()=>{
 const browser=await chromium.launch({executablePath:await binary.executablePath(),args:binary.args,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  await page.route('https://glassplatform.invalid/**',r=>r.abort());
  await page.addInitScript(()=>{window.chrome=window.chrome||{};window.chrome.webview={postMessage(){},addEventListener(type,fn){window.receiveHost=fn;}};});
  await page.goto('file://'+path.resolve(__dirname,'../res/slideshow.html'));
  for(const locale of ['ru','en'])for(const mode of ['unlock-modules','unlock-processes']){
   await page.evaluate(({locale,mode})=>window.receiveHost({data:{type:'state',locale,page:mode,holdReport:Array.from({length:18},(_,i)=>`process${i}.exe (86) (PID ${1000+i}): GlassCompat.dll, GlassCore.dll, WndCompat.dll`).join('\n')}}),{locale,mode});
   const geometry=await page.evaluate(()=>{
    const copy=document.querySelector('#unlock-panel .unlock-copy').getBoundingClientRect();
    const report=document.getElementById('unlock-holders').getBoundingClientRect();
    const list=document.getElementById('unlock-holder-list');
    return {copyBottom:copy.bottom,reportTop:report.top,reportBottom:report.bottom,rowHeight:document.querySelector('.unlock-holder').getBoundingClientRect().height,scroll:list.scrollHeight>list.clientHeight};
   });
   assert.ok(geometry.copyBottom<geometry.reportTop,JSON.stringify(geometry));
   assert.ok(geometry.reportBottom<=540);assert.ok(geometry.scroll);assert.ok(geometry.rowHeight<=40,JSON.stringify(geometry));
  }
  console.log('PASS: Chromium RU/EN unlock modules/processes: copy and scrollable holder report do not overlap');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
