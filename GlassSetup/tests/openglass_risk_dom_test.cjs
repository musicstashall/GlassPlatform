// Production HTML: risk is not corruption, does not prevent removal, and stays visible in selected cards.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
let receive;const messages=[],errors=[];const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8'),{
 url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(t,cb){receive=cb;},postMessage(m){messages.push(m);}}};}
});
const d=dom.window.document;
const send=(components,locale='ru',extra={})=>receive({data:{type:'state',appMode:'setup',page:'components',locale,components,componentsLoading:false,...extra}});
for(const locale of ['ru','en']){
 const warning=locale==='ru'?'Опасность применения: сбои DWM, чёрный экран, возврат к экрану входа.':'Danger: DWM crashes, black screens, return to sign-in.';
 const og={id:'openglass',title:'OpenGlass',installed:false,checked:true,applySupported:true,operation:'install',
  targetVersion:'3.0.2.3749',targetPayloadVersion:'3.0.2.3749',compatibilityRisk:true,warning};
 // A forbidden metadata request exposes the same retry control, no install intent.
 const forbidden={id:'openglass',title:'OpenGlass',installed:false,checked:false,applySupported:false,metadataBlocked:true,metadataError:'GitHub HTTP error (403)'};
 send([forbidden],locale);
 let forbiddenCard=d.querySelector('#components-grid [data-product="openglass"]');
 const metadataRetry=forbiddenCard.querySelector('.download-retry');assert.ok(metadataRetry);
 assert.ok(forbiddenCard.textContent.includes('403'));
 assert.equal(forbiddenCard.querySelector('.comp-decision').disabled,true);
 const prior=messages.length;metadataRetry.click();assert.equal(messages.at(-1),'metadata-retry:openglass');assert.equal(messages.length,prior+1);
 dom.window.commitComponentChoice(d.getElementById('product-openglass'),true);
 assert.equal(d.getElementById('product-openglass').checked,false);
 assert.ok(d.getElementById('components-gesture-status').textContent.includes('403'));
 send([{...forbidden,metadataRetrying:true}],locale);
 assert.equal(d.querySelector('#components-grid .download-retry').disabled,true);
 send([{...forbidden,metadataBlocked:false,metadataRetrying:false,applySupported:true,targetPayloadVersion:'3.0.2.3749'}],locale);
 assert.equal(d.querySelector('#components-grid .download-retry'),null);
 assert.equal(d.querySelector('#components-grid .comp-decision').disabled,false);
 // Reproduce log4: both vendor API calls failed, no live versions.
 const offlineOg={...og,checked:false,targetVersion:'',targetPayloadVersion:'',remotePayloadVersion:'',displayVersion:'3.0.2.3749',versionFallback:true};
 const offlineSut={id:'secureUxTheme',title:'SecureUxTheme',installed:false,checked:false,applySupported:true,targetVersion:'',displayVersion:'4.0.0',versionFallback:true};
 send([offlineOg,offlineSut],locale);
 const offline=d.querySelector('#components-grid [data-product="openglass"]');
 assert.ok(offline.classList.contains('compatibility-risk'),'available OG warns before selection despite API failure');
 assert.ok(offline.textContent.includes('3.0.2.3749'));
 assert.equal(dom.window.getComputedStyle(offline.querySelector('.comp-warning-button')).color,'rgb(255, 107, 107)');
 assert.ok(!offline.textContent.includes('на сервере 3.0.2.3749')&&!offline.textContent.includes('on server 3.0.2.3749'));
 assert.ok(d.querySelector('#components-grid [data-product="secureUxTheme"]').textContent.includes('4.0.0'));
 send([{...offlineSut,checked:true}],locale);
 assert.ok(d.querySelector('#components-installed-grid [data-product="secureUxTheme"]').textContent.includes('4.0.0'));
 send([og],locale);
 const card=d.querySelector('#components-installed-grid [data-product="openglass"]');
 assert.ok(card.classList.contains('compatibility-risk'));assert.ok(!card.classList.contains('is-broken'));
 const warn=card.querySelector('.comp-warning-button');assert.ok(warn);warn.click();
 assert.ok(d.getElementById('components-warning-dialog').textContent.includes(warning));
 d.getElementById('components-warning-close').click();
 send([{...og,checked:false,compatibilityRisk:false,warning:''}],locale);
 assert.equal(d.querySelectorAll('.compatibility-risk').length,0);
 send([{...og,installed:true,operation:'remove',removeSupported:true}],locale);
 assert.equal(d.querySelectorAll('.compatibility-risk').length,0,'never make removal look like dangerous apply');
 const many=Array.from({length:7},(_,i)=>({id:'p'+i,title:'Product '+i,installed:true,checked:false,applySupported:true,targetVersion:'1.0.0'}));
 send(many,locale);assert.equal(d.querySelectorAll('#installed-foot .c-more').length,0);
 assert.ok(!/ещё|more/.test(d.getElementById('installed-foot').textContent));
 send(many.map(item=>({...item,installed:false})),locale);
 assert.equal(d.getElementById('components-scroll-hint').textContent,'','no redundant available overflow hint');
 const dependent={id:'settings64',title:'GlassSettings',installed:true,checked:false,applySupported:true,warning:'Dependent product retained without its dependency'};
 send([dependent],locale);
 const retained=d.querySelector('#components-installed-grid [data-product="settings64"]');
 assert.ok(retained.querySelector('.comp-warning-button'));
 assert.ok(!retained.classList.contains('has-warning')&&!retained.classList.contains('has-offer'),'kept installed dependencies keep neutral surface');
 send([{...dependent,installed:false}],locale);
 assert.ok(d.querySelector('#components-grid [data-product="settings64"]').classList.contains('has-warning'),'available warning styling unchanged');
 const source={id:'x64',title:'Glass',installed:false,checked:false,applySupported:false,applyHint:locale==='ru'?'Нет подходящего источника MSI':'No eligible MSI source'};
 send([source],locale);
 const input=d.getElementById('product-x64'),drag={id:'x64',input,source:'available'};
 const note=dom.window.componentDropText(drag,'installed');assert.equal(note,source.applyHint);
 assert.ok(!/Дождитесь|Wait for/.test(note));
 assert.equal(d.querySelector('#components-grid .comp-decision').disabled,true,'do not bypass the source gate');
 const ids=['openglass','openglassResources','secureUxTheme','x64','settings64'];
 send(ids.map(id=>({id,title:id,checked:true,installed:false,applySupported:true,operation:'install'})),locale);
 assert.deepEqual([...d.querySelectorAll('#components-plan-items li[data-product]')].map(x=>x.dataset.product),
  ['settings64','x64','secureUxTheme','openglassResources','openglass']);
 assert.ok(!/cannot be applied|нельзя применить/.test(d.getElementById('components-plan-notices').textContent));
 send([source],locale,{componentsLoading:true});assert.match(dom.window.componentDropText(drag,'installed'),/Проверяются|Checking/);
}
assert.deepEqual(errors,[]);dom.window.close();console.log('PASS: risk red-state/icon/dialog, removal/undo, no installed overflow hint, unavailable vs pending source diagnostics and gate retained');
