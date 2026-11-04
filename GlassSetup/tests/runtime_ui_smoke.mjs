// Runs only the HTML fixture on an already-running preview, never Windows products.
// GLASS_PREVIEW_NODE_MODULES may point to an external playwright-core installation.
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const modules=process.env.GLASS_PREVIEW_NODE_MODULES;
const load=name=>modules?pathToFileURL(path.join(modules,name)).href:name;
const {chromium:playwright}=await import(load(modules?'playwright-core/index.mjs':'playwright-core'));
let options={headless:true};
if(process.env.GLASS_PREVIEW_SPARTICUZ==='1') {
 const {default:chromium}=await import(load(modules?'@sparticuz/chromium/build/index.js':'@sparticuz/chromium'));
 options={...options,executablePath:await chromium.executablePath(),args:chromium.args.filter(a=>a!=='--single-process')};
} else if(process.env.GLASS_PREVIEW_CHROMIUM)options.executablePath=process.env.GLASS_PREVIEW_CHROMIUM;
const browser=await playwright.launch(options);
const page=await browser.newPage({viewport:{width:1100,height:800}});const errors=[];page.on('pageerror',e=>errors.push(e.message));
for(const os64 of [true,false]) for(const removal of [false,true]) {
 await page.goto((process.env.GLASS_PREVIEW_URL||'http://127.0.0.1:8080')+'/runtime-products-preview.html'+('?os='+(os64?'x64':'x86')+'&mode='+(removal?'unins#uninstall':'setup')));
 const frame=page.frameLocator('#runtime');await frame.locator('#product-x86').waitFor({state:'visible'});
 if(await frame.locator('#components-grid input').count()!==(os64?5:3))throw Error('product count');
 const list=await frame.locator('.components-list').evaluate(el=>el.getBoundingClientRect().height);if(Math.abs(list-330)>.01)throw Error('list '+list);
 if(!removal){if(await frame.locator('#components-grid input:checked').count()!==(os64?4:2))throw Error('fresh defaults');await frame.locator('#product-settings').uncheck();await frame.locator('#product-secureUxTheme').check();}
 else {await frame.locator('#product-settings').check();}
 const sutDesc=await frame.locator('.comp-card').filter({has:frame.locator('#product-secureUxTheme')}).locator('.comp-desc').textContent();
 if(!sutDesc.includes('System32')||sutDesc.includes('ответственность'))throw Error('SUT description');
 const p32Desc=await frame.locator('.comp-card').filter({has:frame.locator('#product-x86')}).locator('.comp-desc').textContent();
 if(p32Desc.includes('оболочки Windows')===os64)throw Error('x86 shell suffix');
 if(os64){const og=frame.locator('.comp-card').filter({has:frame.locator('#product-openglass')});if(!await og.evaluate(el=>el.classList.contains('has-warning')))throw Error('OpenGlass manual-management warning missing');}
 await frame.locator('.comp-warning-button').first().click();await frame.locator('#components-warning-dialog').waitFor({state:'visible'});await page.keyboard.press('Escape');await frame.locator('#components-warning-dialog').waitFor({state:'hidden'});
 if((await page.locator('#messages').textContent())==='cancel')throw Error('warning Escape cancelled wizard');
 await frame.locator('#components-select-all').click();
 if(await frame.locator('#components-grid input:checked').count()!==(os64?5:3))throw Error('select all');
 if(removal&&await frame.locator('#product-cleanup').isChecked())throw Error('bulk cleanup');
 console.log('PASS runtime '+(os64?'OS x64 ':'OS x86 ')+(removal?'Unins':'Setup')+`: host messages, 330px list, ${os64?5:3} independent cards, warning/Esc, select-all excluding cleanup`);
}
for(const locale of ['ru','en'])for(const note of ['cache','startup','none']) {
 await page.goto((process.env.GLASS_PREVIEW_URL||'http://127.0.0.1:8080')+'/runtime-products-preview.html?page=complete&locale='+locale+'&note='+note);
 const frame=page.frameLocator('#runtime');await frame.locator('#complete-panel').waitFor({state:'visible'});
 const title=await frame.locator('#complete-title').textContent(),body=await frame.locator('#complete-body').textContent();
 if(note!=='none') {
  if(title!==(locale==='ru'?'Установка завершена с замечаниями':'Installation completed with notes'))throw Error('neutral completion title: '+title);
  if(!body.includes(locale==='ru'?'завершающим действиям установщика':'final setup actions'))throw Error('neutral completion description');
  const detail=await frame.locator('#complete-warn-list').textContent();
  if(note==='cache'&&(!detail.includes('validate-packages-parent')||!detail.includes('C:\\ProgramData\\Packages')||!detail.includes('0x00000005')))throw Error('cache details missing');
  const fits=await frame.locator('#complete-panel .complete-copy').evaluate(el=>{const r=el.getBoundingClientRect();return r.bottom<=540&&r.right<=1000;});if(!fits)throw Error('completion copy clipped');
 } else if((await frame.locator('#complete-panel').getAttribute('class')||'').includes('has-warnings')||title.includes('notes')||title.includes('замечаниями'))throw Error('clean completion gained warning');
 console.log('PASS completion '+locale+' '+note+': neutral title/body, details and 1000x540 content');
}
if(errors.length)throw Error(errors.join('\n'));await browser.close();
