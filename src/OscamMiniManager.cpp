#include "OscamMiniManager.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
std::string t(std::string s){
    s.erase(s.begin(),std::find_if(s.begin(),s.end(),[](unsigned char c){return !std::isspace(c);}));
    s.erase(std::find_if(s.rbegin(),s.rend(),[](unsigned char c){return !std::isspace(c);}).base(),s.end());
    return s;
}
std::map<std::string,std::string> sec(const std::string& txt,const std::string& wanted){
    std::map<std::string,std::string> out; std::istringstream in(txt); std::string line; bool active=false;
    while(std::getline(in,line)){
        auto p=line.find_first_of("#;"); if(p!=std::string::npos) line.resize(p);
        line=t(line); if(line.empty()) continue;
        if(line.front()=='['&&line.back()==']'){active=t(line.substr(1,line.size()-2))==wanted;continue;}
        if(!active) continue;
        auto e=line.find('='); if(e==std::string::npos) continue;
        out[t(line.substr(0,e))]=t(line.substr(e+1));
    }
    return out;
}
std::vector<std::map<std::string,std::string>> readerSecs(const std::string& txt){
    std::vector<std::map<std::string,std::string>> out; std::istringstream in(txt); std::string line;
    std::map<std::string,std::string>* cur=nullptr;
    while(std::getline(in,line)){
        auto p=line.find_first_of("#;"); if(p!=std::string::npos) line.resize(p);
        line=t(line); if(line.empty()) continue;
        if(line.front()=='['&&line.back()==']'){
            if(t(line.substr(1,line.size()-2))=="reader"){out.emplace_back();cur=&out.back();} else cur=nullptr;
            continue;
        }
        if(!cur) continue;
        auto e=line.find('='); if(e==std::string::npos) continue;
        (*cur)[t(line.substr(0,e))]=t(line.substr(e+1));
    }
    return out;
}
std::string gv(const std::map<std::string,std::string>&m,const std::string&k,const std::string&d=""){
    auto i=m.find(k); return i==m.end()?d:i->second;
}
int gi(const std::map<std::string,std::string>&m,const std::string&k,int d){
    try{return std::stoi(gv(m,k,std::to_string(d)));}catch(...){return d;}
}
bool truth(std::string s){
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});
    return s=="1"||s=="true"||s=="yes"||s=="on";
}
std::string ini(std::string s){
    s.erase(std::remove(s.begin(),s.end(),'\r'),s.end());
    s.erase(std::remove(s.begin(),s.end(),'\n'),s.end());
    return s;
}
}

OscamMiniManager& OscamMiniManager::instance(){ static OscamMiniManager x; return x; }
std::string OscamMiniManager::trim(const std::string&v){return t(v);}
bool OscamMiniManager::validHex(const std::string&v,size_t n){if(n&&v.size()!=n)return false;return !v.empty()&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isxdigit(c);});}
bool OscamMiniManager::validToken(const std::string&v,size_t n){return !v.empty()&&v.size()<=n&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-'||c=='.'||c=='@';});}
bool OscamMiniManager::validDevice(const std::string&v){return v.rfind("/dev/ttyUSB",0)==0||v.rfind("/dev/ttyACM",0)==0||v.rfind("/dev/serial/by-id/",0)==0;}
std::string OscamMiniManager::readFile(const std::string&p){std::ifstream f(p);if(!f)return{};std::ostringstream s;s<<f.rdbuf();return s.str();}
bool OscamMiniManager::writeAtomic(const std::string&p,const std::string&c,std::string&e){
    try{fs::create_directories(fs::path(p).parent_path());auto tmp=p+".tmp."+std::to_string(getpid());{std::ofstream f(tmp);if(!f){e="cannot create temp file";return false;}f<<c;}fs::rename(tmp,p);return true;}
    catch(const std::exception&x){e=x.what();return false;}
}
std::string OscamMiniManager::runCommand(const std::string&c,int*code){
    std::array<char,512>b{};std::string out;FILE*p=popen((c+" 2>&1").c_str(),"r");
    if(!p){if(code)*code=-1;return"popen failed";}while(fgets(b.data(),(int)b.size(),p))out+=b.data();int rc=pclose(p);
    if(code)*code=WIFEXITED(rc)?WEXITSTATUS(rc):-1;return out;
}
std::vector<std::string> OscamMiniManager::ttyDevices(){
    std::vector<std::string>o;try{
        if(fs::exists("/dev/serial/by-id"))for(auto&e:fs::directory_iterator("/dev/serial/by-id"))o.push_back(e.path().string());
        for(auto&e:fs::directory_iterator("/dev")){auto n=e.path().filename().string();if(n.rfind("ttyUSB",0)==0||n.rfind("ttyACM",0)==0)o.push_back(e.path().string());}
    }catch(...){}
    std::sort(o.begin(),o.end());o.erase(std::unique(o.begin(),o.end()),o.end());return o;
}
OscamMiniManager::Settings OscamMiniManager::loadSettingsLocked(){
    Settings s;
    auto n=sec(readFile(std::string(kConfigDir)+"/oscam.conf"),"newcamd");
    s.bindIp=gv(n,"serverip","127.0.0.1");s.newcamdPorts=gv(n,"port","");s.newcamdKey=gv(n,"key",s.newcamdKey);s.keepalive=truth(gv(n,"keepalive","1"));
    auto u=sec(readFile(std::string(kConfigDir)+"/oscam.user"),"account");
    s.user=gv(u,"user","tvstreamer");s.password=gv(u,"pwd","tvstreamer");s.userGroups=gv(u,"group","1,2");s.au=truth(gv(u,"au","1"));
    for(auto&m:readerSecs(readFile(std::string(kConfigDir)+"/oscam.server"))){
        Reader r;r.label=gv(m,"label","Reader");r.protocol=gv(m,"protocol","mouse");r.device=gv(m,"device","");r.caid=gv(m,"caid","");
        r.detect=gv(m,"detect","cd");r.mhz=gi(m,"mhz",600);r.cardmhz=gi(m,"cardmhz",600);r.group=gi(m,"group",1);
        r.ident=gv(m,"ident","");r.emmcache=gv(m,"emmcache","1,3,2");r.enabled=!truth(gv(m,"disable","0"));s.readers.push_back(r);
    }
    return s;
}
bool OscamMiniManager::saveSettingsLocked(const Settings&s,std::string&e){
    if(!validHex(s.newcamdKey,28)){e="Newcamd key must be 28 hex chars";return false;}
    if(!validToken(s.user)||s.password.empty()){e="invalid user/password";return false;}
    if(s.newcamdPorts.empty()){e="invalid ports";return false;}
    std::ostringstream c,u,sv;
    c<<"[global]\nlogfile = stdout\nclienttimeout = 5000\nclientmaxidle = 120\nwaitforcards = 1\n\n[newcamd]\nserverip = "<<ini(s.bindIp)<<"\nport = "<<ini(s.newcamdPorts)<<"\nkey = "<<ini(s.newcamdKey)<<"\nkeepalive = "<<(s.keepalive?1:0)<<"\n";
    u<<"[account]\nuser = "<<ini(s.user)<<"\npwd = "<<ini(s.password)<<"\ngroup = "<<ini(s.userGroups)<<"\nau = "<<(s.au?1:0)<<"\nallowedprotocols = newcamd\n";
    for(auto&r:s.readers){
        if(!validDevice(r.device)||!validHex(r.caid,4)){e="invalid reader "+r.label;return false;}
        sv<<"[reader]\nlabel = "<<ini(r.label)<<"\nprotocol = "<<ini(r.protocol)<<"\ndevice = "<<ini(r.device)<<"\ncaid = "<<ini(r.caid)<<"\ndetect = "<<ini(r.detect)
          <<"\nmhz = "<<r.mhz<<"\ncardmhz = "<<r.cardmhz<<"\ngroup = "<<r.group<<"\n";
        if(!r.ident.empty())sv<<"ident = "<<ini(r.ident)<<"\n";
        if(!r.emmcache.empty())sv<<"emmcache = "<<ini(r.emmcache)<<"\n";
        if(!r.enabled)sv<<"disable = 1\n";
        sv<<"\n";
    }
    return writeAtomic(std::string(kConfigDir)+"/oscam.conf",c.str(),e)&&writeAtomic(std::string(kConfigDir)+"/oscam.user",u.str(),e)&&writeAtomic(std::string(kConfigDir)+"/oscam.server",sv.str(),e);
}
Json::Value OscamMiniManager::statusLocked(){
    Json::Value r;r["config_dir"]=kConfigDir;r["binary_exists"]=fs::exists(kBinary);
    int rc=0;auto a=trim(runCommand("systemctl is-active "+std::string(kService),&rc));r["service_active"]=(a=="active");r["service_state"]=a.empty()?"unknown":a;
    Json::Value d(Json::arrayValue);for(auto&x:ttyDevices())d.append(x);r["devices"]=d;
    r["process"]=trim(runCommand("ps -C oscam-mini -o pid=,rss=,vsz=,%cpu=,cmd="));
    auto l=runCommand("journalctl -u "+std::string(kService)+" -n 30 --no-pager -o cat");if(l.size()>12000)l=l.substr(l.size()-12000);r["log"]=l;return r;
}
Json::Value OscamMiniManager::parseJson(const std::string&b,std::string&e){Json::Value v;Json::CharReaderBuilder x;std::unique_ptr<Json::CharReader>r(x.newCharReader());if(!r->parse(b.data(),b.data()+b.size(),&v,&e))return Json::Value();return v;}
std::string OscamMiniManager::jsonString(const Json::Value&v){Json::StreamWriterBuilder b;b["indentation"]="";return Json::writeString(b,v);}
std::string OscamMiniManager::statusJson(){std::lock_guard<std::mutex>g(mutex_);return jsonString(statusLocked());}
std::string OscamMiniManager::settingsJson(){
    std::lock_guard<std::mutex>g(mutex_);auto s=loadSettingsLocked();Json::Value r;r["bind_ip"]=s.bindIp;r["ports"]=s.newcamdPorts;r["key"]=s.newcamdKey;r["keepalive"]=s.keepalive;r["user"]=s.user;r["password"]=s.password;r["groups"]=s.userGroups;r["au"]=s.au;
    Json::Value a(Json::arrayValue);for(auto&x:s.readers){Json::Value q;q["label"]=x.label;q["protocol"]=x.protocol;q["device"]=x.device;q["caid"]=x.caid;q["detect"]=x.detect;q["mhz"]=x.mhz;q["cardmhz"]=x.cardmhz;q["group"]=x.group;q["ident"]=x.ident;q["emmcache"]=x.emmcache;q["enabled"]=x.enabled;a.append(q);}r["readers"]=a;return jsonString(r);
}
std::string OscamMiniManager::saveSettingsJson(const std::string&body){
    std::lock_guard<std::mutex>g(mutex_);std::string e;auto j=parseJson(body,e);Json::Value o;if(!e.empty()){o["ok"]=false;o["error"]=e;return jsonString(o);}
    Settings s;s.bindIp=j.get("bind_ip","127.0.0.1").asString();s.newcamdPorts=j.get("ports","").asString();s.newcamdKey=j.get("key","0102030405060708091011121314").asString();s.keepalive=j.get("keepalive",true).asBool();s.user=j.get("user","tvstreamer").asString();s.password=j.get("password","tvstreamer").asString();s.userGroups=j.get("groups","1,2").asString();s.au=j.get("au",true).asBool();
    if(j["readers"].isArray())for(auto&x:j["readers"]){Reader r;r.label=x.get("label","Reader").asString();r.protocol=x.get("protocol","mouse").asString();r.device=x.get("device","").asString();r.caid=x.get("caid","").asString();r.detect=x.get("detect","cd").asString();r.mhz=x.get("mhz",600).asInt();r.cardmhz=x.get("cardmhz",600).asInt();r.group=x.get("group",1).asInt();r.ident=x.get("ident","").asString();r.emmcache=x.get("emmcache","1,3,2").asString();r.enabled=x.get("enabled",true).asBool();s.readers.push_back(r);}
    if(!saveSettingsLocked(s,e)){o["ok"]=false;o["error"]=e;return jsonString(o);}int rc=0;runCommand("systemctl restart "+std::string(kService),&rc);o["ok"]=(rc==0);o["status"]=statusLocked();return jsonString(o);
}
std::string OscamMiniManager::serviceActionJson(const std::string&body){
    std::lock_guard<std::mutex>g(mutex_);std::string e;auto j=parseJson(body,e);Json::Value o;auto a=j.get("action","").asString();
    if(a!="start"&&a!="stop"&&a!="restart"){o["ok"]=false;o["error"]="unsupported action";return jsonString(o);}
    int rc=0;runCommand("systemctl "+a+" "+std::string(kService),&rc);o["ok"]=(rc==0);o["status"]=statusLocked();return jsonString(o);
}
std::string OscamMiniManager::renderPage(){
    return R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8"><title>OSCam-mini</title><style>
body{font-family:Arial;background:#0f1218;color:#eee;margin:0}.w{max-width:1000px;margin:auto;padding:18px}.c{background:#161b25;border:1px solid #2a3241;border-radius:16px;padding:16px;margin:12px 0}.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}label{display:flex;flex-direction:column;gap:5px}input,select{background:#0e131c;border:1px solid #334058;color:#fff;border-radius:8px;padding:9px}button,a{border:0;border-radius:9px;padding:9px 14px;background:#1f8bff;color:#fff;text-decoration:none}.alt{background:#30394a}.r{border-top:1px solid #30394a;padding-top:12px;margin-top:12px}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}pre{white-space:pre-wrap;background:#0b0f16;padding:10px;border-radius:9px;max-height:260px;overflow:auto}</style></head><body><div class="w">
<div class="row"><a class="alt" href="/">← TVStreammerSAT5</a><h2>OSCam-mini</h2><span id="state"></span></div>
<div class="c"><div class="row"><button onclick="act('start')">Старт</button><button class="alt" onclick="act('restart')">Перезапуск</button><button class="alt" onclick="act('stop')">Стоп</button></div><small>/opt/TVStreammerSAT5/oscam-mini/config</small></div>
<div class="c"><h3>Newcamd</h3><div class="g"><label>Bind IP<input id="bind_ip"></label><label>Ports<input id="ports"></label><label>DES key<input id="key"></label><label>User<input id="user"></label><label>Password<input id="password" type="password"></label><label>Groups<input id="groups"></label></div></div>
<div class="c"><div class="row"><h3>Phoenix readers</h3><button onclick="addReader()">+ Ридер</button></div><div id="readers"></div></div>
<div class="c"><button onclick="save()">Сохранить и перезапустить</button><span id="msg"></span></div><div class="c"><pre id="proc"></pre><pre id="log"></pre></div></div>
<script>
let devices=[];async function api(u,o){return (await fetch(u,o)).json()}
function addReader(r={}){let i=document.querySelectorAll('.r').length;let d=document.createElement('div');let ops=['',...devices].map(x=>`<option ${x===r.device?'selected':''}>${x}</option>`).join('');d.innerHTML=`<div class="r"><div class="g"><label>Label<input class="label" value="${r.label||'Reader'+(i+1)}"></label><label>Device<select class="device">${ops}</select></label><label>CAID<input class="caid" value="${r.caid||'0652'}"></label><label>Protocol<select class="protocol"><option>mouse</option><option>phoenix</option></select></label><label>Detect<input class="detect" value="${r.detect||'cd'}"></label><label>MHz<input class="mhz" type="number" value="${r.mhz||600}"></label><label>Card MHz<input class="cardmhz" type="number" value="${r.cardmhz||600}"></label><label>Group<input class="group" type="number" value="${r.group||1}"></label><label>Ident<input class="ident" value="${r.ident||''}"></label><label>EMM cache<input class="emmcache" value="${r.emmcache||'1,3,2'}"></label></div><button class="alt" onclick="this.parentElement.remove()">Удалить</button></div>`;readers.append(...d.childNodes)}
async function loadAll(){let st=await api('/api/oscam-mini/status');devices=st.devices||[];state.textContent=st.service_active?'● работает':'● '+st.service_state;proc.textContent=st.process||'not running';log.textContent=st.log||'';let s=await api('/api/oscam-mini/settings');for(let k of ['bind_ip','ports','key','user','password','groups'])document.getElementById(k).value=s[k]||'';readers.innerHTML='';(s.readers||[]).forEach(addReader)}
async function save(){let rs=[...document.querySelectorAll('.r')].map(e=>({label:e.querySelector('.label').value,device:e.querySelector('.device').value,caid:e.querySelector('.caid').value,protocol:e.querySelector('.protocol').value,detect:e.querySelector('.detect').value,mhz:+e.querySelector('.mhz').value,cardmhz:+e.querySelector('.cardmhz').value,group:+e.querySelector('.group').value,ident:e.querySelector('.ident').value,emmcache:e.querySelector('.emmcache').value,enabled:true}));let b={bind_ip:bind_ip.value,ports:ports.value,key:key.value,user:user.value,password:password.value,groups:groups.value,keepalive:true,au:true,readers:rs};let x=await api('/api/oscam-mini/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});msg.textContent=x.ok?'Сохранено':x.error;loadAll()}
async function act(a){await api('/api/oscam-mini/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:a})});loadAll()}loadAll();
</script></body></html>)HTML";
}
