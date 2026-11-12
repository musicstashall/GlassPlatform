// Every supported production WebView mode, both localizations and both applications.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
const modes=[...html.match(/const valid=\[(.*?)\]/)[1].matchAll(/'([^']+)'/g)].map(m=>m[1]);
let receive;const errors=[];const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
const dom=new JSDOM(html,{url:'https://glassplatform.invalid/',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:true,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.chrome={webview:{addEventListener(t,cb){receive=cb;},postMessage(){}}};}});
for(const locale of ['ru','en'])for(const appMode of ['setup','unins'])for(const page of modes){
 receive({data:{type:'state',locale,appMode,page,components:[],componentsLoading:false,errorReason:'Diagnostic test',cleanupLines:[],conflicts:[],downloads:[]}});
 const actual=dom.window.document.getElementById('stage').dataset.mode;
 // Welcome may legitimately wait for local slideshow image readiness in JSDOM.
 assert.ok(actual===page||(page==='welcome'&&actual==='loading'),`${appMode}/${locale}/${page}: ${actual}`);
}
assert.deepEqual(errors,[]);dom.window.close();
console.log(`PASS: ${modes.length} production WebView modes x RU/EN x Setup/GlassUnins render without JS errors (DOM smoke, not Windows visual acceptance)`);
