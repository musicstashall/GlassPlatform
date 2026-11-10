// Chromium geometry for production package progress; no Setup/network execution.
const {chromium}=require('playwright-core'),pkg=require('@sparticuz/chromium');
const binary=pkg.default||pkg,path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({executablePath:await binary.executablePath(),args:binary.args,headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  await page.route('https://glassplatform.invalid/**',r=>r.abort());
  await page.addInitScript(()=>{window.chrome={webview:{postMessage(){},addEventListener(t,f){window.receive=f;}}};});
  await page.goto('file://'+path.resolve(__dirname,'../res/slideshow.html'));
  for(const locale of ['ru','en']){
   await page.evaluate(locale=>window.receive({data:{type:'state',page:'components',locale,components:['x64','x86','settings64','settings32','openglass','secureUxTheme'].map(id=>({id,title:id,checked:true,installed:true,installedVersion:'2.0.0.105',targetVersion:'2.0.0.106',operation:'update'})),downloads:['x64','x86','settings64','settings32','openglass','secureUxTheme'].map(id=>({id,phase:'downloading',network:true,total:10485760,bytes:5242880,elapsedMs:2000}))}}),locale);
   await page.waitForTimeout(400);
   const geometry=await page.evaluate(()=>{
    const lane=document.querySelector('.c-lane.installed').getBoundingClientRect();
    return {extraStrip:!!document.getElementById('components-downloads'),laneBottom:lane.bottom,bars:document.querySelectorAll('#components-installed-grid .product-transfer').length,
     rows:[...document.querySelectorAll('#components-installed-grid .c-card')].map(card=>{const bar=card.querySelector('.product-transfer').getBoundingClientRect(),rect=card.getBoundingClientRect();return bar.left>=rect.left&&bar.right<=rect.right&&bar.top>=rect.top&&bar.bottom<=rect.bottom;})};
   });
   assert.equal(geometry.extraStrip,false);assert.ok(geometry.laneBottom<=540,JSON.stringify(geometry));assert.equal(geometry.bars,6);assert.ok(geometry.rows.every(Boolean));
   await page.evaluate(()=>window.receive({data:{type:'state',downloads:[]}}));
   assert.equal(await page.locator('#components-panel').evaluate(el=>el.classList.contains('has-downloads')),false);
  }
  console.log('PASS: Chromium RU/EN six card bars fit; no extra aggregate strip; existing native footer owns overall progress');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exit(1);});
