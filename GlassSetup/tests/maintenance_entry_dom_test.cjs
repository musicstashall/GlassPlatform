// Actual production HTML. Animation API is a seam, not a rendered-frame test.
const {JSDOM,VirtualConsole}=require('jsdom');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const html=fs.readFileSync(path.join(__dirname,'../res/slideshow.html'),'utf8');
for(const reduced of [false,true]){
 let receive;const animations=[],errors=[];
 const vc=new VirtualConsole();vc.on('jsdomError',e=>errors.push(e));
 const dom=new JSDOM(html,{url:'https://glassplatform.invalid/?lang=ru',runScripts:'dangerously',virtualConsole:vc,
 beforeParse(w){w.matchMedia=()=>({matches:reduced,addEventListener(){},removeEventListener(){}});w.CSS={supports:()=>true};
 w.HTMLElement.prototype.animate=function(frames,options){animations.push({id:this.dataset.product,frames,options});return {cancel(){}};};
 w.chrome={webview:{addEventListener(type,callback){if(type==='message')receive=callback;},postMessage(){}}};}});
 const d=dom.window.document,rows=['x64','x86','settings64','openglass'].map((id,i)=>({id,title:id,description:'Product',installed:i<3,checked:false,operation:i<3?'repair':'install',installedVersion:'1.0.0',targetVersion:'1.0.0'}));
 const send=(page,extra={})=>receive({data:{type:'state',page,maintenance:true,components:rows,...extra}});
 send('checking',{componentsLoading:true});assert.equal(animations.length,0);
 send('components',{componentsLoading:true});assert.equal(animations.length,0);
 send('components',{componentsLoading:false});
 assert.equal(d.querySelector('.c-slot-left').classList.contains('is-hidden'),true);
 assert.equal(d.querySelector('.c-slot-left').getAttribute('aria-hidden'),'true');
 assert.equal(d.querySelector('[data-component-edge="available"]').getAttribute('aria-hidden'),'false');
 assert.equal(animations.length,reduced?0:3);
 if(!reduced){assert.deepEqual(animations.map(a=>a.options.delay),[0,65,130]);assert.ok(animations.every(a=>a.options.duration>0&&a.frames[0].opacity===0&&a.frames[1].opacity===1));}
 const count=animations.length;send('components');assert.equal(animations.length,count); // no replay on ordinary host refresh
 send('components',{components:rows.map(x=>({...x,checked:x.id==='x64'}))});assert.equal(animations.length,count); // no replay on choice
 send('checking');send('components');assert.equal(animations.length,reduced?0:count+3); // explicit re-entry
 assert.deepEqual(errors.map(e=>e.message),[]);dom.window.close();
}
process.stdout.write('PASS: hidden Available on entry; real production cascade scheduling, delayed inspection result, state refresh/re-entry, reduced motion\n');
