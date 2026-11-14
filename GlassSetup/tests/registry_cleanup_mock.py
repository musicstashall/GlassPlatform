"""Run actual CleanOwnedSettings with fake registry, not a replacement policy.
Upstream audit: ALTaleX531/OpenGlass fb145fcd391b274cb91e642c93b3fc2b0e30e61c,
Scripts/OpenGlass.iss DeleteConfig: 54 names, minus five Windows color values.
"""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'GlassShared/src/RegistrySanity.cpp').read_text()
rules=s[s.index('constexpr Rule kDwmOpenGlassRules[]'):s.index('bool IsIncorrect(')]
# Independent inventory transcribed from the audited upstream DeleteConfig.
upstream='''ColorizationColor ColorizationColorOverride ColorizationColorInactive ColorizationAfterglow ColorizationColorBalance ColorizationAfterglowBalance ColorizationBlurBalance ColorizationAfterglowOverride ColorizationColorBalanceOverride ColorizationAfterglowBalanceOverride ColorizationBlurBalanceOverride GlassOpacity GlassOpacityInactive ColorizationColorCaption ColorizationColorCaptionInactive ColorizationColorCaptionMaximized ColorizationColorCaptionInactiveMaximized ColorizationOpaqueBlend ColorizationBaseTransparent ColorizationBaseMaximized ColorizationBaseOpaque ColorizationOpaqueBlendPriority ColorizationOpacity ColorizationOpacityInactive ColorizationOpacityMaximized ColorizationOpacityInactiveMaximized GlassType GlassOverrideAccent CustomThemeReflection ColorizationGlassReflectionIntensity ColorizationGlassReflectionOpacity ColorizationGlassReflectionOpacityInactive ColorizationGlassReflectionOpacityMaximized ColorizationGlassReflectionOpacityInactiveMaximized ColorizationGlassReflectionParallaxIntensity ColorizationGlassReflectionPolicy BlurDeviation BlurOptimization RoundRectRadius CustomThemeMaterial MaterialOpacity UseDirect3DRendering CaptionButtons CenterCaption TextGlowMode CustomThemeAtlas DisableModernBorders DisableGlassOnBattery DisabledHooks EnableUxMitigation GlassSafetyZoneMode MINMAXBUTTONGLOWid CLOSEBUTTONGLOWid TOOLCLOSEBUTTONGLOWid'''.split()
protected={'ColorizationColor','ColorizationAfterglow','ColorizationColorBalance','ColorizationAfterglowBalance','ColorizationBlurBalance'}
assert set(re.findall(r'L"([^"]+)"',rules))==set(upstream)-protected
start=s.index('namespace {\nbool VerifyTreeAccess')
# Fake handles use integer IDs; only null-handle initializers are adapted.
production=s[start:s.rindex('} // namespace Shared::RegistrySanity')].replace('HKEY child=nullptr','HKEY child=0').replace('HKEY glass=nullptr','HKEY glass=0').replace('HKEY dwm=nullptr','HKEY dwm=0')
prefix=r'''
#include <string>
#include <map>
#include <set>
#include <tuple>
#include <cassert>
#include <cwchar>
using DWORD=unsigned;using LONG=long;using REGSAM=unsigned;using HKEY=int;
constexpr LONG ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_ACCESS_DENIED=5,ERROR_NO_MORE_ITEMS=259;
constexpr REGSAM KEY_ALL_ACCESS=1,KEY_QUERY_VALUE=2,KEY_SET_VALUE=4,KEY_WOW64_64KEY=256,KEY_WOW64_32KEY=512;
constexpr REGSAM kViews[]={KEY_WOW64_64KEY,KEY_WOW64_32KEY};
enum class ValueKind {Dword,DwordBool,Text};struct Rule{const wchar_t* valueName;ValueKind kind;};
constexpr Rule kGlassPlatformRootRules[]={{L"InstallPath",ValueKind::Text}};
int _wcsicmp(const wchar_t* a,const wchar_t* b){return wcscasecmp(a,b);}
using Key=std::pair<HKEY,REGSAM>;
std::map<Key,std::set<std::wstring>> registry;
std::map<int,Key> handles;
std::set<Key> opened;
int serial=100,deletions=0;bool denyOpen=false,denyDelete=false,rewrite=false,denyQuery=false;
LONG RegOpenKeyExW(HKEY root,const wchar_t* path,DWORD,REGSAM access,HKEY* out){
 if(std::wstring(path)!=L"Software\\Microsoft\\Windows\\DWM")return ERROR_FILE_NOT_FOUND;
 Key key={root,access&(KEY_WOW64_64KEY|KEY_WOW64_32KEY)};
 if(denyOpen)return ERROR_ACCESS_DENIED;
 if(!registry.count(key))return ERROR_FILE_NOT_FOUND;
 opened.insert(key);*out=++serial;handles[*out]=key;return ERROR_SUCCESS;
}
LONG RegCloseKey(HKEY h){assert(handles.erase(h)==1);return ERROR_SUCCESS;}
LONG RegEnumKeyExW(HKEY,DWORD,wchar_t*,DWORD*,void*,void*,void*,void*){return ERROR_NO_MORE_ITEMS;}
LONG RegEnumValueW(HKEY,DWORD,wchar_t*,DWORD*,void*,void*,void*,void*){return ERROR_NO_MORE_ITEMS;}
LONG RegDeleteTreeW(HKEY,const wchar_t*){assert(false);return 5;}
LONG RegDeleteValueW(HKEY h,const wchar_t* name){
 if(denyDelete)return ERROR_ACCESS_DENIED;
 auto& values=registry.at(handles.at(h));if(!values.erase(name))return ERROR_FILE_NOT_FOUND;
 ++deletions;if(rewrite)values.insert(name);return ERROR_SUCCESS;
}
LONG RegQueryValueExW(HKEY h,const wchar_t* name,void*,void*,void*,void*){
 if(denyQuery)return ERROR_ACCESS_DENIED;
 return registry.at(handles.at(h)).count(name)?ERROR_SUCCESS:ERROR_FILE_NOT_FOUND;
}
'''
main=r'''
void reset(){assert(handles.empty());registry.clear();opened.clear();deletions=0;denyOpen=denyDelete=rewrite=denyQuery=false;
 for(int hive:{1,2,3})for(auto view:kViews){auto& values=registry[{hive,view}];
 for(auto& rule:kDwmOpenGlassRules)values.insert(rule.valueName);
 for(auto name:{L"ColorizationColor",L"ColorizationAfterglow",L"ColorizationColorBalance",L"ColorizationAfterglowBalance",L"ColorizationBlurBalance",L"EnableTransparency",L"DwmInitSessionActivityId_test",L"GradientWhitePixelGPUBlacklist",L"OneCoreNoBootDWM",L"FutureUnknown"})values.insert(name);
 }}
int main(){std::wstring detail;reset();auto before=registry;
 for(int hive:{1,2,3}){assert(CleanPlatformSettings(hive,hive==3,false,detail));assert(CleanPlatformSettings(hive,hive==3,true,detail));}
 assert(registry==before&&deletions==0&&opened.empty()); // Installer delegates OG; no DWM probe/write.

 for(int hive:{1,2,3})assert(CleanOwnedSettings(hive,hive==3,false,detail));
 assert(registry==before&&deletions==0&&opened.size()==6);
 for(int hive:{1,2,3})assert(CleanOwnedSettings(hive,hive==3,true,detail));
 assert(deletions==49*6&&handles.empty());
 for(auto& [key,values]:registry){(void)key;assert(values.size()==10);for(auto& rule:kDwmOpenGlassRules)assert(!values.count(rule.valueName));}
 assert(CleanOwnedSettings(3,true,true,detail)); // Already absent is success.
 reset();denyOpen=true;assert(!CleanOwnedSettings(1,false,false,detail)&&deletions==0&&handles.empty());
 reset();denyDelete=true;assert(!CleanOwnedSettings(3,true,true,detail)&&deletions==0&&handles.empty());assert(detail.find(L"ColorizationColorOverride")!=std::wstring::npos);
 reset();rewrite=true;assert(!CleanOwnedSettings(1,false,true,detail)&&handles.empty());assert(detail.find(L"verification")!=std::wstring::npos);
 reset();denyQuery=true;assert(!CleanOwnedSettings(1,false,true,detail)&&handles.empty());
 reset();registry.clear();assert(CleanOwnedSettings(1,false,true,detail));
}
'''
with tempfile.TemporaryDirectory() as temp:
 p=Path(temp)/'registry.cpp';p.write_text(prefix+rules+production+main);exe=Path(temp)/'test'
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual registry cleanup: 49 upstream-owned DWM names, all user/machine roots and views, consent preflight, preserved Windows/unknown values, access/delete/query errors and rewritten-value detection')
