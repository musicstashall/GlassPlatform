// Real Chromium layout checks against the approved concept C, using production HTML.
// Optional external deps: playwright-core; CHROMIUM_PATH or @sparticuz/chromium.
// No Glass executable/service is launched; browser IPC/discovery is a fixture.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('playwright-core');
const root=path.resolve(__dirname,'../..');
(async()=>{
 const packaged=process.env.CHROMIUM_PATH?null:(require('@sparticuz/chromium').default||require('@sparticuz/chromium'));
 const browser=await chromium.launch({executablePath:process.env.CHROMIUM_PATH||await packaged.executablePath(),args:packaged?packaged.args:[],headless:true,ignoreDefaultArgs:['--hide-scrollbars']});
 try {
  const page=await browser.newPage({viewport:{width:1000,height:540},reducedMotion:'reduce'});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{window.__messages=[];window.chrome={webview:{addEventListener(t,fn){if(t==='message')window.__receive=fn;},postMessage(m){window.__messages.push(m);}}};});
  await page.route('**/*',async route=>{
   const u=new URL(route.request().url());let file;
   if(u.pathname==='/')file=path.join(root,'GlassSetup/res/slideshow.html');
   else if(u.pathname==='/assets/banner.png')file=path.join(root,'GlassSetup/res/welcome_banner.png');
   else if(u.pathname.startsWith('/assets/'))file=path.join(root,'GlassSetup/res/assets',path.basename(u.pathname));
   else file=path.resolve(root,'.'+u.pathname);
   if(!file.startsWith(root+path.sep)||!fs.existsSync(file)||!fs.statSync(file).isFile())return route.fulfill({status:404,body:''});
   await route.fulfill({path:file});
  });
  await page.goto('https://glassplatform.invalid/');
  const products=[['x64','GlassPlatform x64'],['x86','GlassPlatform x86'],['settings64','GlassSettings x64'],['settings32','GlassSettings x86'],['openglass','OpenGlass'],['secureUxTheme','SecureUxTheme']].map(([id,title],i)=>({id,title,description:'Product information',installed:i<3,installedVersion:i<3?'1.0.0.0':'',targetVersion:'1.1.0.0',checked:i===2,operation:i<3?'update':'install',warning:i===1?'Architecture-specific warning':'',state:i===2?'Обновить':'Без изменений',reasons:[]}));
  await page.evaluate(components=>window.__receive({data:{type:'state',locale:'ru',page:'components',maintenance:true,components}}),products);
  await page.locator('#components-panel').waitFor({state:'visible'});
  await page.waitForFunction(()=>Math.abs(document.querySelector('.c-lane.available').getBoundingClientRect().y-document.querySelector('#stage').getBoundingClientRect().y-156)<.1);
  const read=()=>page.evaluate(()=>{
   const rect=s=>{const a=document.querySelector(s).getBoundingClientRect(),b=document.querySelector('#stage, .stage').getBoundingClientRect();return {x:Math.round(a.x-b.x),y:Math.round(a.y-b.y),w:Math.round(a.width),h:Math.round(a.height)};};
   return {available:rect('.c-lane.available'),installed:rect('.c-lane.installed'),plan:rect('.c-plan'),card:rect('.c-card')};
  });
  const skin=()=>page.evaluate(()=>{
   const card=document.createElement('article');card.className='comp-card c-card';document.querySelector('.c-lane-body').append(card);
   const c=getComputedStyle(card),lane=getComputedStyle(document.querySelector('.c-lane')),plan=getComputedStyle(document.querySelector('.c-plan'));
   const result={cardRadius:c.borderRadius,cardBackground:c.backgroundImage,cardShadow:c.boxShadow,laneRadius:lane.borderRadius,laneBackground:lane.backgroundImage,planRadius:plan.borderRadius,planBackground:plan.backgroundImage};card.remove();return result;
  });
  const actual=await read(),actualSkin=await skin();
  assert.deepEqual(actual.available,{x:48,y:156,w:302,h:372});
  assert.deepEqual(actual.installed,{x:366,y:156,w:302,h:372});
  assert.deepEqual(actual.plan,{x:684,y:156,w:268,h:372});
  assert.equal(actual.card.h,72);
  await page.locator('#product-settings32').locator('..').locator('.comp-decision').click();
  assert.equal(await page.evaluate(()=>window.__messages.at(-1)),'component:settings32:1');
  assert.equal(await page.locator('#components-installed-grid #product-settings32').count(),1);
  await page.locator('#product-settings32').locator('..').locator('.comp-name').click();
  await assert.equal(await page.locator('#components-warning-dialog').isVisible(),true);
  await page.keyboard.press('Escape');
  assert.equal(await page.locator('#components-warning-dialog').isVisible(),false);
  const out=process.env.GLASS_UI_SCREENSHOT_DIR;
  await page.evaluate(()=>document.activeElement.blur());
  if(out){fs.mkdirSync(out,{recursive:true});await page.locator('#stage').screenshot({path:path.join(out,'production-maintenance.png')});}
  await page.setViewportSize({width:1180,height:800});
  await page.goto('https://glassplatform.invalid/GlassSetup/docs/mockups/product-maintenance-concepts.html');
  await page.evaluate(()=>document.querySelector('[data-pin="available"]').click());
  const reference=await read();assert.deepEqual(actualSkin,await skin(),'concept C card/lane/plan skins');
  for(const key of ['available','installed','plan'])assert.deepEqual(actual[key],reference[key],key+' must match approved concept C');
  assert.equal(actual.card.h,reference.card.h);
  if(out)await page.locator('.stage').screenshot({path:path.join(out,'reference-concept-c.png')});
  assert.deepEqual(errors,[]);
  console.log('PASS: real Chromium: concept C shelf/plan rectangles and 72px cards, live Settings IPC/movement, product information and Escape. Not Windows/WebView runtime or a full pixel parity assertion.');
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
