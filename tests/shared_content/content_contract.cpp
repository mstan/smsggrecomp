#undef NDEBUG
extern "C" {
#include "glue.h"
#include "game_spec.h"
}
#include "content_mods.h"
#include "fixture_identity.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static int counter,before;
static int increment(void*){before=counter;counter++;return 1;}
static void finish_counter(void*,int committed){if(!committed)counter=before;}
static void register_hooks(){assert(glue_register_mod_hook("fixture.increment-counter",increment,finish_counter,nullptr));}
extern "C" const GameSpec g_game_spec={"Synthetic shared-services consumer","fixture",FIXTURE_CRC,16384,GAME_PLATFORM_SMS,register_hooks,nullptr,nullptr,nullptr};
extern "C" void call_by_address(uint16_t){assert(false&&"This fixture never executes guest CPU code");}
static int begin(void*){return glue_mod_begin();}
static int read(void*,const char *space,uint64_t at,uint8_t *out,size_t n){return glue_mod_read(space,at,out,n);}
static int write(void*,const char *space,uint64_t at,const uint8_t *bytes,size_t n){return glue_mod_write(space,at,bytes,n);}
static int hook(void*,const char *id){return glue_mod_hook(id);}
static void finish(void*,int committed){glue_mod_finish(committed);}
static void reset(){assert(glue_load_rom("target.sms"));glue_init(false,0);counter=0;}
static void cartridge(const unsigned char expected[4]){unsigned char bytes[4];assert(glue_mod_read("cartridge",0x300,bytes,4));assert(!std::memcmp(bytes,expected,4));}
static void unchanged(const char *path,const char *expected){FILE *f=std::fopen(path,"rb");assert(f);std::fseek(f,0,SEEK_END);long size=std::ftell(f);assert(size>0);std::rewind(f);std::vector<unsigned char> data((size_t)size);assert(std::fread(data.data(),1,data.size(),f)==data.size());std::fclose(f);unsigned char digest[32];assert(rmod_sha256_bytes(data.data(),data.size(),digest));char encoded[65];for(int i=0;i<32;i++)std::sprintf(encoded+2*i,"%02x",digest[i]);assert(!std::strcmp(encoded,expected));}
int main(int argc,char **){
 if(argc>1){
  sms_content_load();assert(sms_content_count()==3);reset();assert(sms_content_prepare("target.sms"));
  auto *mods=rmod_create();rmod_engine engine={nullptr,"fixture",FIXTURE_SHA,begin,read,write,hook,finish};unsigned char identity[32],ram[4];
  assert(sms_content_apply(mods,&engine,identity)==1);assert(glue_mod_read("ram",256,ram,4));const unsigned char expected[4]={0xca,0xfe,0xba,0xbe};assert(!std::memcmp(ram,expected,4));rmod_destroy(mods);
  std::puts("PASS: fresh-process saved content selection and original-image revalidation");return 0;
 }
 sms_content_load();assert(sms_content_count()==0);
 assert(!sms_content_install("fixture.invalid.smsmod"));assert(sms_content_count()==0);
 assert(sms_content_install("fixture.good.smsmod"));assert(sms_content_install("fixture.rollback.smsmod"));
 assert(sms_content_install("fixture.8k.smsmod"));
 SmsContentInfo info;assert(sms_content_get(0,&info)&&!info.enabled&&info.has_source);
 assert(!sms_content_import("fixture.good","target.sms"));assert(sms_content_import("fixture.good","donor.sms"));
 assert(sms_content_import("fixture.rollback","donor.sms"));
 reset();auto *mods=rmod_create();assert(mods);rmod_engine engine={nullptr,"fixture",FIXTURE_SHA,begin,read,write,hook,finish};
 unsigned char fingerprint[32]={0},again[32]={0};const unsigned char original[4]={0x11,0x22,0x33,0x44},overlay[4]={0xde,0xad,0xbe,0xef};
 assert(sms_content_prepare("target.sms"));assert(sms_content_apply(mods,&engine,fingerprint)==1);cartridge(original);assert(!counter&&!glue_get_pad1());
 assert(sms_content_enable("fixture.good",1));assert(sms_content_prepare("target.sms"));assert(sms_content_apply(mods,&engine,fingerprint)==1);
 cartridge(overlay);assert(counter==1&&glue_get_pad1()==64);
 assert(sms_content_apply(mods,&engine,again)==0);cartridge(overlay);assert(counter==1&&glue_get_pad1()==64);
 reset();assert(sms_content_import("fixture.good","same-donor-other-path.sms"));assert(sms_content_prepare("target.sms"));assert(sms_content_apply(mods,&engine,again)==1);assert(!std::memcmp(fingerprint,again,32));
 reset();assert(sms_content_enable("fixture.good",0));assert(sms_content_enable("fixture.rollback",1));assert(sms_content_prepare("target.sms"));
 assert(sms_content_apply(mods,&engine,again)==0);cartridge(original);assert(counter==0&&glue_get_pad1()==0);
 assert(sms_content_enable("fixture.good",1));assert(sms_content_prepare("target.sms"));assert(sms_content_apply(mods,&engine,again)==0);cartridge(original);assert(!counter&&!glue_get_pad1());
 assert(sms_content_enable("fixture.rollback",0));assert(!sms_content_prepare("donor.sms"));
 assert(sms_content_enable("fixture.good",0));reset();
 assert(sms_content_import("fixture.8k",u8"donor-é.sms"));assert(sms_content_enable("fixture.8k",1));assert(sms_content_prepare("target.sms"));assert(sms_content_apply(mods,&engine,again)==1);
 unsigned char ram[4];const unsigned char ram_overlay[4]={0xca,0xfe,0xba,0xbe};assert(glue_mod_read("ram",256,ram,4)&&!std::memcmp(ram,ram_overlay,4));cartridge(original);assert(!counter&&!glue_get_pad1());
 unchanged("target.sms",FIXTURE_SHA);unchanged("donor.sms",DONOR_SHA);rmod_destroy(mods);
 std::puts("PASS: original-image ZIP descriptor/import, nontrivial cartridge overlay, target/expected guards, conflict rejection, path-independent fingerprint, native hook commit and full byte/private-state rollback");return 0;
}
