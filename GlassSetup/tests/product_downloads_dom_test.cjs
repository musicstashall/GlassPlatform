// Actual production DOM, no network or Windows operations.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const errors=[],messages=[];let receive;
const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8'),{
 url:'https://glassplatform.invalid/?lang=ru',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};w.chrome={webview:{addEventListener(t,fn){receive=fn;},postMessage(m){messages.push(m);}}};}
});
const d=dom.window.document,send=data=>receive({data:{type:'state',...data}});
for(const locale of ['ru','en']){
 const components=['x64','x86','settings64','settings32','openglass','secureUxTheme'].map(id=>({id,title:id,checked:true,installed:true,operation:'update',installedVersion:'1.0',targetVersion:'2.0',remoteVersion:'2.0',applySupported:true}));
 send({locale,page:'components',components:components.map((c,i)=>({...c,downloadRequired:i!==0})),downloads:[]});
 assert.equal(d.querySelectorAll('.product-transfer').length,0,'selection does not create progress before Next');
 assert.ok(d.querySelector('#product-x64').closest('.c-card').classList.contains('download-local'));
 assert.ok(d.querySelector('#product-settings64').closest('.c-card').classList.contains('download-planned'));
 send({locale,page:'components',components,downloads:components.map(c=>({id:c.id,phase:'preparing',total:0,bytes:0}))});
 assert.equal(d.querySelectorAll('#components-installed-grid .product-transfer').length,6);
 assert.equal(d.querySelector('#components-downloads'),null,'aggregate progress uses the existing native footer strip');
 const jobs=components.map((c,i)=>({id:c.id,phase:'downloading',total:(i+1)*1024,bytes:(i+1)*512,elapsedMs:2000,network:true}));
 const cancel=d.querySelector('#components-plan-items .plan-cancel');cancel.focus();
 send({downloads:jobs});
 assert.equal(d.activeElement,cancel,'progress must not replace focused plan controls');
 assert.match(d.querySelector('#components-plan-items .plan-transfer-status').textContent,/50%/);
 assert.equal(d.querySelector('#components-installed-grid .product-transfer i').style.width,'50%');
 assert.equal(d.querySelector('.product-transfer').style.getPropertyValue('--transfer-progress'),'50%');
 assert.ok(d.querySelector('.product-transfer').classList.contains('is-transferring'));
 const css=d.querySelector('style').textContent;
 assert.match(css,/clip-path:inset\(0 calc\(100% - var\(--transfer-progress,0%\)\)/);
 assert.match(css,/@keyframes product-transfer-shine\{0%\{transform:translateX\(-100%\)\}50%,100%\{transform:translateX\(100%\)\}\}/);
 assert.match(css,/animation:product-transfer-shine 3\.2s linear infinite/);
 assert.match(css,/prefers-reduced-motion:reduce/);
 // Removing the plan row cancels the same native selection IPC and hides stale progress immediately.
 cancel.click();assert.equal(messages.at(-1),'component:settings64:0');
 assert.equal(d.querySelector('#product-settings64').checked,false);
 assert.equal(d.querySelector('#components-plan-items li[data-product="settings64"]'),null);
 const failed=jobs.map(j=>({...j,phase:'error',error:'HTTP 404'}));send({downloads:failed});
 const retry=d.querySelector('#components-installed-grid .download-retry');assert.ok(retry);assert.equal(d.querySelectorAll('.product-transfer.is-transferring').length,0);retry.click();assert.match(messages.at(-1),/^download-retry:/);
 send({downloads:jobs.map(j=>({...j,phase:'verifying',bytes:j.total}))});
 assert.equal(d.querySelectorAll('.product-transfer.is-transferring').length,0);
 assert.match(d.querySelector('#components-plan-items .plan-transfer-status').textContent,locale==='ru'?/Проверка/:/Verifying/);
 assert.ok(!d.querySelector('#components-plan-items .plan-transfer-status').textContent.includes('100%'));
 send({downloads:jobs.map(j=>({...j,phase:'ready',bytes:j.total}))});
 assert.equal(d.querySelectorAll('#components-installed-grid .product-transfer[aria-valuenow="100"]').length,5);
 assert.equal(d.querySelectorAll('#components-installed-grid .download-local').length,5);
 send({downloads:[]});assert.equal(d.querySelector('#components-downloads'),null);
 assert.equal(d.querySelectorAll('#components-installed-grid .product-transfer').length,0);
}
assert.equal(errors.length,0,errors.map(String).join('\n'));dom.window.close();
console.log('PASS: production RU/EN six product transfers, per-product progress, no extra aggregate strip, stable focus, cancel IPC, error retry, verification and ready states');
