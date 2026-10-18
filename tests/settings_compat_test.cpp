#include "core/config.h"
#include "core/legacy_holsters.h"
#include "core/log.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <cstdlib>
#include <cwchar>
#include <sstream>
#include <cctype>

namespace fs = std::filesystem;
static fs::path test_dir;
static unsigned checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; std::exit(1); }
}
static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void write(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary); f << s;
}
static std::map<std::string, std::string> parse(const std::string& text) {
    std::map<std::string, std::string> out;
    std::istringstream lines(text);
    std::string line, section;
    auto trim = [](std::string s) {
        auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string{} : s.substr(a, b-a+1);
    };
    auto lower = [](std::string s) { for (auto& c:s) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    while (std::getline(lines,line)) {
        line=trim(line);
        if (line.empty() || line[0]==';' || line[0]=='#') continue;
        if (line.front()=='[' && line.back()==']') { section=lower(trim(line.substr(1,line.size()-2))); continue; }
        auto eq=line.find('=');
        if (eq==std::string::npos) continue;
        auto v=trim(line.substr(eq+1));
        if (auto pos=v.find(" ;");pos!=std::string::npos) v=trim(v.substr(0,pos));
        out[section+"."+lower(trim(line.substr(0,eq)))]=v;
    }
    return out;
}
namespace rdrvr::log {
void info(const char*, ...) {}
void warn(const char*, ...) {}
const wchar_t* path_in_game_dir(const wchar_t* name, wchar_t* buf, size_t len) {
    const auto p=(test_dir/name).wstring();
    wcsncpy_s(buf,len,p.c_str(),_TRUNCATE);
    return buf;
}
}

int wmain(int argc, wchar_t** argv) {
    check(argc==2,"scratch directory required");
    test_dir=argv[1];
    fs::create_directories(test_dir);
    // Scratch files only: even a failed reset test cannot reach the actual user INI.
    // Synthetic profiles only. No external settings, game files or personal paths.
    const std::string shipped = R"ini([Render]
RenderScale=100
RenderHeadset=off
RenderResolution=off
[Holsters]
LowerBackEnabled=1
KnifeEnabled=0
KnifeWeapon=auto
RightHipRadius=0.15
[Hands]
DualWield=1
DualWieldCopy=1
[Gestures]
Lasso=1
)ini";
    const std::string user = R"ini(; Synthetic user overrides
[Render]
RenderScale=125
RenderHeadset=index
RenderResolution=1788
[Holsters]
LowerBackOffset=0.070 -0.030 -0.250
Anchor=body
[Hands]
DualWield=0
DualWieldCopy=0
[Body]
BodyFollowsHead=1
[Example]
Untouched=arbitrary text
)ini";
    check(!shipped.empty()&&!user.empty(),"synthetic profiles initialized");
    write(test_dir/L"RDRVR.ini",shipped);
    write(test_dir/L"RDRVR.user.ini",user);
    const auto user_path=(test_dir/L"RDRVR.user.ini").wstring();
    rdrvr::config::set_user_path(user_path.c_str());
    auto values=parse(shipped);
    for (const auto& [k,v]:parse(user)) values[k]=v;
    for (const auto& [k,v]:values) {
        const auto dot=k.find('.');
        check(rdrvr::config::get_string(k.substr(0,dot).c_str(),k.substr(dot+1).c_str(),"<missing>")==v,"existing effective value preserved");
    }
    check(rdrvr::config::legacy_holsters(),"old layout detected");
    check(rdrvr::config::get_int("Render","RenderScale",100)==125,"saved scale retained");
    check(rdrvr::config::get_string("Holsters","LowerBackOffset","")=="0.070 -0.030 -0.250","rear offset retained exactly");
    check(rdrvr::config::get_string("Render","RenderHeadset","")=="index","headset selection retained");
    check(rdrvr::config::get_int("Render","RenderResolution",0)==1788,"render height retained");
    check(!rdrvr::config::get_bool("Hands","DualWield",true),"single gun retained");
    check(!rdrvr::config::get_bool("Hands","DualWieldCopy",true),"copy disabled");
    check(!rdrvr::config::get_bool("Gestures","FistsBothTriggers",true),"existing melee gesture not gated");
    check(!rdrvr::config::get_bool("Gestures","Guard",true),"new guard input not enabled implicitly");
    check(!rdrvr::config::get_bool("Gestures","TipHat",true),"new hat input not enabled implicitly");
    check(rdrvr::config::get_string("Gestures","LassoThrow","")=="swing","legacy lasso swing retained");
    for (int i=0;i<3;++i) rdrvr::config::load();
    check(read(test_dir/L"RDRVR.user.ini")==user,"user file unchanged on reload");
    check(read(test_dir/L"RDRVR.ini")==shipped,"shipped file unchanged");
    // Accidental exposure to a reset tag must not discard the preserved legacy layout.
    write(test_dir/L"RDRVR.ini",shipped+"\n[Compat]\nSettingsReset=compat-test\n");
    rdrvr::config::load();
    check(read(test_dir/L"RDRVR.user.ini")==user,"reset tag cannot wipe legacy profile");
    check(!fs::exists(test_dir/L"RDRVR.user.ini.before-compat-test.bak"),"no migration backup made");
    check(rdrvr::config::get_int("Render","RenderScale",100)==125,"resolution survives reset tag");
    // Explicit newer preferences win over compatibility fallbacks.
    const std::string explicit_user=user+"\n[Gestures]\nFistsBothTriggers=1\nGuard=1\nTipHat=1\nLassoThrow=both\n";
    write(test_dir/L"RDRVR.user.ini",explicit_user);
    rdrvr::config::load();
    check(rdrvr::config::get_bool("Gestures","FistsBothTriggers",false),"explicit trigger mode wins");
    check(rdrvr::config::get_bool("Gestures","Guard",false),"explicit guard wins");
    check(rdrvr::config::get_bool("Gestures","TipHat",false),"explicit hat mode wins");
    check(rdrvr::config::get_string("Gestures","LassoThrow","")=="both","explicit lasso mode wins");
    check(read(test_dir/L"RDRVR.user.ini")==explicit_user,"explicit values not rewritten");
    // New profiles keep upstream layout and defaults; old keys alone do not override a new layout.
    write(test_dir/L"RDRVR.ini","[Holsters]\nLassoOffset=0 0 0\n");
    write(test_dir/L"RDRVR.user.ini",user);
    rdrvr::config::load();
    check(!rdrvr::config::legacy_holsters(),"new lasso layout wins");
    check(rdrvr::config::get_bool("Gestures","Guard",true),"new profile defaults untouched");
    const char* olds[]={"LowerBackOffset","LowerBackRadius","LowerBackEnabled","LowerBackWeapon"};
    const char* news[]={"LassoOffset","LassoRadius","LassoEnabled","LassoWeapon"};
    for (const char* old:olds) {
        std::set<std::string> keys={old};
        check(rdrvr::legacy_holsters::selected([&](const char* k){return keys.contains(k);}),"each legacy key recognized");
        for(const char* modern:news) {
            keys.insert(modern);
            check(!rdrvr::legacy_holsters::selected([&](const char* k){return keys.contains(k);}),"explicit current key respected");
            keys.erase(modern);
        }
    }
    struct Zone {const char* key; const char* label; int bone; int slots[3]; float off[3]; float radius; int weapon=-1; bool enabled=true;};
    Zone zones[8]{};
    zones[0].bone=0; zones[0].slots[0]=1; zones[0].off[0]=0.23f;
    rdrvr::legacy_holsters::defaults(zones);
    check(std::string(zones[7].key)=="LowerBack","old key still used for load and save");
    check(zones[7].bone==5,"rear knife anchored to drawn pelvis, not rope or sheath");
    check(zones[7].slots[0]==0&&zones[7].slots[1]==3&&zones[7].slots[2]==-1,"knife first, lasso fallback");
    check(zones[7].radius==0.16f&&zones[7].off[1]==0.05f&&zones[7].off[2]==-0.12f,"legacy unset defaults retained");
    check(!zones[3].enabled&&zones[3].slots[1]==3,"old near-hip slot disabled by default");
    check(zones[0].bone==0&&zones[0].slots[0]==1&&zones[0].off[0]==0.23f,"gun layout untouched");
    std::cout << "Settings compatibility: " << checks << " checks passed; synthetic profile files unchanged\n";
}
