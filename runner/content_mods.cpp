/* SMS/GG-specific descriptor consumer over the shared ZIP and transaction core.
 * All cartridge sources are original images opened read-only. No extracted
 * retail files, host paths in fingerprints, or executable plugin loading. */
#include "content_mods.h"
extern "C" {
#include "game_spec.h"
#include "toml.h"
#include "../common/sms_cartridge.h"
#include "recomp_persistence/atomic_file.h"
}
#include "recomp_mod/archive.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace {
namespace fs=std::filesystem;
struct Plan {
 std::string id,name,description,descriptor_sha,target_sha,donor_sha,space,source_path;
 size_t donor_size=0,source_offset=0,target_offset=0,size=0;
 std::vector<unsigned char> expected,replacement;
 std::vector<std::string> hooks;
 bool enabled=false,verified=false;
};
std::vector<Plan> plans;
std::string error,prepared_original;
bool loaded=false,catalog_failed=false;
bool fail(const std::string& why){error=why;return false;}
bool id_ok(const std::string& id){return !id.empty()&&id.size()<64&&std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_';});}
bool sha_ok(const std::string& hash){return hash.size()==64&&std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string hash(const void *data,size_t size){unsigned char digest[32];if(!rmod_sha256_bytes(data,size,digest))return {};std::string out;for(auto value:digest){out+="0123456789abcdef"[value>>4];out+="0123456789abcdef"[value&15];}return out;}
bool read(const fs::path& path,std::vector<unsigned char>& out,size_t limit){
 std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return fail("Cannot read original image or mod descriptor");
 auto length=input.tellg();if(length<0||(uint64_t)length>limit)return fail("Image or descriptor exceeds its supported extent");
 out.resize((size_t)length);input.seekg(0);if(!out.empty()&&!input.read((char*)out.data(),(std::streamsize)out.size()))return fail("Original image read failed");return true;
}
std::string text(toml_table_t *table,const char *key){auto value=toml_string_in(table,key);if(!value.ok)return {};std::string out=value.u.s;free(value.u.s);return out;}
bool number(toml_table_t *table,const char *key,size_t& out){auto value=toml_int_in(table,key);if(!value.ok||value.u.i<0)return false;out=(size_t)value.u.i;return true;}
int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
bool decode(const std::string& input,std::vector<unsigned char>& out){if(input.empty()||(input.size()&1)||input.size()>8192)return false;out.resize(input.size()/2);for(size_t i=0;i<out.size();i++){int a=nibble(input[2*i]),b=nibble(input[2*i+1]);if(a<0||b<0)return false;out[i]=(unsigned char)((a<<4)|b);}return true;}
bool parse(const fs::path& path,Plan& plan){
 std::vector<unsigned char> raw;if(!read(path,raw,32768))return false;plan.descriptor_sha=hash(raw.data(),raw.size());raw.push_back(0);
 char why[256];toml_table_t *root=toml_parse((char*)raw.data(),why,sizeof why);if(!root)return fail("Invalid overlay.toml");
 std::unique_ptr<toml_table_t,decltype(&toml_free)> owner(root,toml_free);
 size_t version=0;bool ok=number(root,"format_version",version)&&version==1;
 plan.id=text(root,"id");plan.name=text(root,"name");plan.description=text(root,"description");plan.target_sha=text(root,"target_sha256");
 ok=ok&&id_ok(plan.id)&&!plan.name.empty()&&plan.name.size()<128&&plan.description.size()<512&&text(root,"game_id")==g_game_spec.short_name&&sha_ok(plan.target_sha);
 auto *overlay=toml_table_in(root,"overlay");
 if(overlay){
  plan.donor_sha=text(overlay,"original_sha256");plan.space=text(overlay,"target_space");
  ok=ok&&sha_ok(plan.donor_sha)&&(plan.space=="ram"||plan.space=="cartridge")&&number(overlay,"original_size",plan.donor_size)&&number(overlay,"source_offset",plan.source_offset)&&number(overlay,"target_offset",plan.target_offset)&&number(overlay,"size",plan.size)&&decode(text(overlay,"expected_hex"),plan.expected);
  ok=ok&&plan.donor_size>=8192&&plan.donor_size<=SMS_CARTRIDGE_MAX+512u&&plan.size&&plan.size<=4096&&plan.expected.size()==plan.size&&plan.source_offset<=plan.donor_size&&plan.size<=plan.donor_size-plan.source_offset;
 }
 auto *hooks=toml_array_in(root,"hook");if(hooks){int count=toml_array_nelem(hooks);if(count>8)ok=false;for(int i=0;i<count&&ok;i++){
  auto *entry=toml_table_at(hooks,i);std::string id=entry?text(entry,"id"):"";if(!id_ok(id))ok=false;else plan.hooks.push_back(id);
 }}
 ok=ok&&(overlay||!plan.hooks.empty());return ok?true:fail("Unsupported descriptor identity, source, target, hook or bounded extent");
}
Plan *find(const char *id){if(!id)return nullptr;for(auto& plan:plans)if(plan.id==id)return &plan;return nullptr;}
bool save(){std::string data;for(const auto& p:plans)data+=p.id+"|"+(p.enabled?"1":"0")+"|"+p.source_path+"\n";return recomp_atomic_file_store("smsgg-content.ini",data.data(),data.size())?true:fail("Cannot save content-mod choices");}
bool import(Plan& plan,const std::string& path){
 if(path.size()>=1024||path.find_first_of("\r\n|")!=std::string::npos)return fail("Invalid or too-long original-image path");
 if(path.empty()){plan.source_path.clear();plan.replacement.clear();plan.verified=false;return true;}
 SmsCartridge image;
 if(!sms_cartridge_read(path.c_str(),&image))return fail("Cannot read a complete supported cartridge image");
 std::unique_ptr<SmsCartridge,decltype(&sms_cartridge_release)> owner(&image,sms_cartridge_release);
 if(image.original_size!=plan.donor_size||hash(image.original,image.original_size)!=plan.donor_sha)return fail("This original cartridge image does not match the feature's supported dump");
 if(!sms_cartridge_has_header(&image))return fail("Select the complete original SMS or Game Gear cartridge image");
 const auto *source=sms_cartridge_original_extent(&image,plan.source_offset,plan.size);
 if(!source)return fail("Donor content exceeds the original image extent");
 plan.replacement.assign(source,source+plan.size);plan.source_path=path;plan.verified=true;return true;
}
}
extern "C" void sms_content_load(void) try {
 if(loaded)return;loaded=true;plans.clear();std::error_code ec;
 fs::path installed="smsgg-mods/installed";
 unsigned visited=0;
 if(fs::exists(installed,ec))for(const auto& entry:fs::directory_iterator(installed,ec)){
  if(ec){catalog_failed=true;break;}
  if(plans.size()>=16||++visited>64){catalog_failed=true;error="Content catalog exceeds the supported profile";break;}
  if(!entry.is_directory(ec)||entry.is_symlink(ec)){catalog_failed=true;error="Unexpected content catalog entry";continue;}
  Plan plan;if(parse(entry.path()/"overlay.toml",plan)&&!find(plan.id.c_str()))plans.push_back(std::move(plan));else catalog_failed=true;
 }
 if(ec){catalog_failed=true;error="Content catalog read failed";}
 std::sort(plans.begin(),plans.end(),[](const Plan& a,const Plan& b){return a.id<b.id;});
 std::ifstream selection("smsgg-content.ini");std::string line;while(std::getline(selection,line)){
  auto a=line.find('|'),b=a==std::string::npos?a:line.find('|',a+1);if(a==std::string::npos||b==std::string::npos)continue;
  if(auto *plan=find(line.substr(0,a).c_str())){plan->enabled=line.substr(a+1,b-a-1)=="1";plan->source_path=line.substr(b+1);}
 }
}catch(...){plans.clear();catalog_failed=true;error="Content catalog read failed";}
extern "C" size_t sms_content_count(void) try {sms_content_load();return plans.size();}catch(...){return 0;}
extern "C" int sms_content_get(size_t i,SmsContentInfo *out) try {sms_content_load();if(!out||i>=plans.size())return 0;const auto& p=plans[i];*out={p.id.c_str(),p.name.c_str(),p.description.c_str(),p.source_path.c_str(),p.enabled?1:0,p.size?1:0,p.verified?1:0};return 1;}catch(...){return fail("Content metadata failed");}
extern "C" int sms_content_install(const char *archive) try {
 sms_content_load();if(!archive||plans.size()>=16)return fail("Content package limit reached");std::vector<unsigned char> data;if(!read(fs::u8path(archive),data,16*1024*1024))return 0;
 std::vector<recomp::mods::ZipEntry> entries;if(!recomp::mods::parse_zip(data,entries,&error))return 0;
 if(entries.size()!=1||entries[0].name!="overlay.toml"||entries[0].size>32768)return fail("An SMS/GG descriptor package contains exactly overlay.toml; retail assets stay in the original image");
 std::string archive_sha=hash(data.data(),data.size());fs::path destination=fs::path("smsgg-mods/installed")/archive_sha;
 std::error_code ec;if(fs::exists(destination,ec))return fail("This content package is already installed");if(ec)return fail("Cannot inspect content package destination");
 /* Exactly one known descriptor file can be created here. Invalid imports
  * remove only that newly owned file and its empty directory, never recurse. */
 auto discard=[&]{std::error_code cleanup;fs::remove(destination/"overlay.toml",cleanup);if(cleanup)catalog_failed=true;cleanup.clear();fs::remove(destination,cleanup);if(cleanup)catalog_failed=true;};
 if(!recomp::mods::extract_zip(data,entries,destination,&error)){discard();return 0;}
 Plan plan;if(!parse(destination/"overlay.toml",plan)){discard();return 0;}
 if(find(plan.id.c_str())){discard();return fail("A content package with this ID is already installed");}
 plans.push_back(std::move(plan));std::sort(plans.begin(),plans.end(),[](const Plan& a,const Plan& b){return a.id<b.id;});return save();
}catch(...){return fail("Content package import failed");}
extern "C" int sms_content_enable(const char *id,int enabled) try {sms_content_load();auto *p=find(id);if(!p)return fail("Unknown content feature");bool old=p->enabled;p->enabled=enabled!=0;if(save()){error.clear();return 1;}p->enabled=old;return 0;}catch(...){return fail("Content selection failed");}
extern "C" int sms_content_import(const char *id,const char *path) try {sms_content_load();auto *p=find(id);if(!p||!p->size||!path)return fail("Unknown original-image resource");Plan old=*p;if(!import(*p,path))return 0;if(save()){error.clear();return 1;}*p=std::move(old);return 0;}catch(...){return fail("Original-image import failed");}
extern "C" int sms_content_prepare(const char *original) try {
 sms_content_load();if(catalog_failed)return fail(error.empty()?"Content catalog requires repair":error);
 SmsCartridge image;if(!original||!sms_cartridge_read(original,&image))return fail("Cannot read a complete supported cartridge image");
 prepared_original=hash(image.original,image.original_size);sms_cartridge_release(&image);
 for(auto& p:plans)if(p.enabled){if(p.target_sha!=prepared_original)return fail("Content feature targets a different original game image");if(p.size&&!import(p,p.source_path))return 0;if(p.size&&!p.verified)return fail("Select the supported original donor image before enabling this content feature");}
 error.clear();return 1;
}catch(...){return fail("Content preparation failed");}
extern "C" int sms_content_apply(rmod_context *context,const rmod_engine *engine,unsigned char identity[32]) try {
 std::vector<rmod_patch> patches;std::vector<rmod_hook> hooks;std::string ticket;
 for(const auto& p:plans)if(p.enabled){
  if(p.target_sha!=prepared_original||(p.size&&!p.verified))return fail("Content preparation does not match this loaded game");
  ticket+=p.id+"\n"+p.descriptor_sha+"\n"+p.donor_sha+"\n";
  if(p.size)patches.push_back({p.space.c_str(),p.target_offset,p.expected.data(),p.replacement.data(),p.size});
  for(const auto& hook:p.hooks)hooks.push_back({hook.c_str()});
 }
 if(ticket.empty())return 1;
 auto fingerprint=hash(ticket.data(),ticket.size());if(!rmod_bind_plan(context,g_game_spec.short_name,prepared_original.c_str(),fingerprint.c_str()))return fail(rmod_error(context));
 int result=rmod_apply(context,patches.data(),patches.size(),hooks.data(),hooks.size(),engine,identity);
 if(result!=1)error=rmod_error(context);return result;
}catch(...){return fail("Content activation failed before publication");}
extern "C" const char *sms_content_error(void){return error.c_str();}
