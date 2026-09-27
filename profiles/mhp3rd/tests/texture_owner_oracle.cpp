#include "native/bridge_contracts.hpp"
#include "overlay_module.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t kReturn=0x08001000u, kManager=0x08200000u, kHeap=0x09000000u;
constexpr std::uint32_t kSp=0x08400800u, kCtor=0x0A0E7460u, kOverlay=0x0A05E600u;
constexpr std::uint32_t kOwnerBytes=0x2F470u, kSlot=0x27C70u;
void require(bool ok,const std::string &why) { if(!ok) throw std::runtime_error(why); }
std::uint32_t word(std::span<const std::uint8_t> b,std::size_t at) {
    require(at<=b.size() && b.size()-at>=4,"Word outside overlay");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8u)|
        (std::uint32_t(b[at+2])<<16u)|(std::uint32_t(b[at+3])<<24u);
}
struct Library {
    void *handle{};
    explicit Library(const char *path) {
        handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        if(!handle) { const char *error=dlerror(); throw std::runtime_error(error?error:"dlopen failed"); }
    }
    ~Library() { if(handle) dlclose(handle); }
    Library(const Library&)=delete;
    Library& operator=(const Library&)=delete;
};
class Gate {
    Runtime aot_, interpreted_;
    std::mt19937 random_{0x4F574E52u};
    std::vector<std::uint8_t> baseline_;
public:
    std::uint64_t calls{}, constructor_cases{}, reset_cases{}, stale_vptr_cases{}, address_reuses{}, command_release_cases{};
    unsigned max_steps{},max_dispatches{};
    std::set<std::uint32_t> executed_pages;
    Gate(const Elf32Image &elf,std::span<const std::uint8_t> overlay,Library &library)
        :aot_(elf.required_ram_size()),interpreted_(elf.required_ram_size()) {
        for(Runtime *r:{&aot_,&interpreted_}) {
            (void)elf.load_and_relocate(r->memory());r->memory().copy_in(kOverlay,overlay);
            r->memory().zero(kOverlay+static_cast<std::uint32_t>(overlay.size()),word(overlay,20u));
        }
        register_generated_functions(aot_);
        auto info_fn=reinterpret_cast<const mhp3rd::OverlayModuleInfo *(*)()>(dlsym(library.handle,"mhp3rd_overlay_info"));
        auto register_fn=reinterpret_cast<void(*)(Runtime&)>(dlsym(library.handle,"mhp3rd_register_overlay"));
        require(info_fn && register_fn,"Overlay exports unavailable");const auto *info=info_fn();
        std::uint64_t hash=14695981039346656037ull;
        for(std::size_t i=0;i<64u+word(overlay,12u);++i) { hash^=overlay[i];hash*=1099511628211ull; }
        require(info && info->abi_version==mhp3rd::kOverlayAbiVersion && info->base==kOverlay &&
            info->code_size==word(overlay,12u) && info->hash==hash,"Overlay module/image identity differs");
        register_fn(aot_);require(aot_.has_function(kCtor),"Constructor not in recompiled overlay");
        baseline_=aot_.memory().bytes();
    }
    AllegrexContext context(std::uint32_t entry,std::uint32_t mirror) {
        AllegrexContext c{};for(auto &r:c.gpr) r=random_();
        for(auto &r:c.fpr) r=std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for(auto &r:c.vfpu) r=std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        c.gpr[0]=0;c.gpr[29]=kSp|mirror;c.gpr[31]=kReturn;c.pc=entry;return c;
    }
    void copy(std::uint32_t address,std::span<const std::uint8_t> b) {
        aot_.memory().copy_in(address,b);interpreted_.memory().copy_in(address,b);
    }
    AllegrexContext run(AllegrexContext c) {
        auto reference=c;unsigned dispatches=0,steps=0;
        do {
            require(aot_.has_function(c.pc),"Unregistered AOT continuation at "+std::to_string(c.pc));
            require(aot_.invoke_isolated_aot(c.pc,c) && !aot_.stopped(),"Original AOT stopped");
        } while(c.pc!=kReturn && ++dispatches<10000u);
        require(c.pc==kReturn,"AOT dispatch budget exhausted");
        do {
            const auto pc=reference.pc;
            require((pc>=0x08804000u && pc<0x08965A00u) || (pc>=kCtor && pc<kCtor+112u),
                    "Interpreter left main code/selected constructor at "+std::to_string(pc));
            executed_pages.insert(pc&~0xFFFu);
            require(interpret_allegrex(interpreted_,reference,1u)==InterpreterExit::Budget &&
                    !interpreted_.stopped(),"Constructor interpreter stopped");
        } while(reference.pc!=kReturn && ++steps<2000000u);
        require(reference.pc==kReturn,"Interpreter budget exhausted");
        require(mhp3rd::native::same_context(c,reference),"Original constructor CPU differs");
        require(aot_.memory().bytes()==interpreted_.memory().bytes(),"Original constructor RAM differs");
        require(aot_.memory().vram_bytes()==interpreted_.memory().vram_bytes(),"Original constructor VRAM differs");
        max_steps=std::max(max_steps,steps+1u);max_dispatches=std::max(max_dispatches,dispatches+1u);++calls;return c;
    }
    void check(std::uint32_t mirror) {
        copy(GuestMemory::kPhysicalBase,baseline_);
        auto init=context(0x08879DA4u,mirror);init.gpr[4]=kManager|mirror;init.gpr[5]=kHeap|mirror;init.gpr[6]=0x100000u;
        (void)run(init);
        auto allocate=context(0x08879F08u,mirror);allocate.gpr[4]=kManager|mirror;allocate.gpr[5]=kOwnerBytes;allocate.gpr[6]=16u;
        const auto owner=run(allocate).gpr[2];require(owner!=0u,"Owner allocation failed");
        auto placement=context(0x088A0EF0u,mirror);placement.gpr[4]=kOwnerBytes;placement.gpr[5]=owner;
        require(run(placement).gpr[2]==owner,"Placement identity changed allocation");
        std::vector<std::uint8_t> dirty(kOwnerBytes,0xA5u);copy(owner,dirty);
        // The actual factory clears this allocation before construction. Leave
        // resource slots patterned to independently test the constructor's fill.
        std::vector<std::uint8_t> base(0x1470u,0u);copy(owner,base);
        const auto before=aot_.memory().bytes();
        auto ctor=context(kCtor,mirror);ctor.gpr[4]=owner;require(run(ctor).gpr[2]==1u,"Constructor return differs");
        const auto &memory=aot_.memory();require(memory.load32(owner)==0x0896FBC8u,"Constructor did not install selected vtable");
        require(memory.load32(owner+0x270u)==0xC4u && memory.load32(owner+0x274u)==1u,"Constructor scalar fields differ");
        std::vector<std::uint8_t> slots(0x2E000u);memory.copy_out(owner+0x1470u,slots);
        require(std::all_of(slots.begin(),slots.end(),[](auto b){return b==0u;}),"Constructor did not clear all resource slots");
        const auto begin=GuestMemory::canonical(owner)-GuestMemory::kPhysicalBase;
        const auto stack=GuestMemory::canonical(kSp)-GuestMemory::kPhysicalBase;
        const auto &after=memory.bytes();
        for(std::size_t i=0;i<after.size();++i)
            if((i<begin || i>=begin+kOwnerBytes) && (i<stack-4096u || i>=stack))
                require(after[i]==before[i],"Constructor wrote outside owner and bounded stack");
        ++constructor_cases;
        auto provider=context(0x088B7DE0u,mirror);provider.gpr[4]=owner;provider.gpr[5]=7u;
        require(run(provider).gpr[2]==owner+kSlot,"Constructed owner provider differs");
        std::array<std::uint8_t,32> state{};state.fill(0xD7u);copy(owner+0x13E8u,state);
        auto reset=context(0x0889D6D0u,mirror);reset.gpr[4]=owner+0x13F0u;(void)run(reset);
        std::array<std::uint8_t,32> checked{};memory.copy_out(owner+0x13E8u,checked);
        for(std::size_t i=0;i<checked.size();++i) require(checked[i]==(i>=8u && i<24u?0u:0xD7u),"State reset footprint differs");
        ++reset_cases;
        auto command_init=context(0x08879DA4u,mirror);command_init.gpr[4]=(kManager+0x100u)|mirror;
        command_init.gpr[5]=0x09200000u|mirror;command_init.gpr[6]=0x100000u;(void)run(command_init);
        auto command_request=context(0x08879DB4u,mirror);command_request.gpr[4]=(kManager+0x100u)|mirror;
        command_request.gpr[5]=108u;command_request.gpr[6]=16u;
        const auto command=run(command_request).gpr[2];require(command!=0u,"Command allocation failed");
        std::array<std::uint8_t,16> command_state{};
        const std::array<std::uint32_t,3> state_words{owner+kSlot,command,3u};
        for(std::size_t i=0;i<state_words.size();++i)
            for(unsigned j=0;j<4u;++j)command_state[i*4u+j]=static_cast<std::uint8_t>(state_words[i]>>(8u*j));
        copy(owner+0x13F0u,command_state);
        auto release_command=context(0x088A3474u,mirror);
        release_command.gpr[4]=(kManager+0x100u)|mirror;release_command.gpr[5]=owner;(void)run(release_command);
        memory.copy_out(owner+0x13F0u,command_state);
        require(std::all_of(command_state.begin(),command_state.end(),[](auto b){return b==0u;}) &&
                memory.load32(command-28u)==0u,"Owner release helper did not free commands and clear state");
        ++command_release_cases;
        auto release=context(0x08879FF0u,mirror);release.gpr[4]=kManager|mirror;release.gpr[5]=owner;
        (void)run(release);
        require(memory.load32(owner)==0x0896FBC8u,"Free unexpectedly cleared the stale vtable pointer");
        ++stale_vptr_cases;
        allocate=context(0x08879F08u,mirror);allocate.gpr[4]=kManager|mirror;
        allocate.gpr[5]=kOwnerBytes;allocate.gpr[6]=16u;
        require(run(allocate).gpr[2]==owner,"Owner allocation address was not reused");
        require(memory.load32(owner)==0x0896FBC8u,"Reallocation unexpectedly cleared stale owner data");
        ++address_reuses;
    }
};
}
int main(int argc,char **argv) {
    try {
        require(argc==5,"usage: texture_owner_oracle EBOOT.ELF lobby.bin lobby.dylib new-report.json");
        require(!std::filesystem::exists(argv[4]) && !std::filesystem::is_symlink(argv[4]),"Output exists");
        require(sha256_file(argv[1])=="55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c","Unsupported ELF");
        require(sha256_file(argv[2])=="c34bf34f5e71993f5f2d20cdc39ec1b965f64b66d46f8d7672b449fba64b5aca","Unsupported lobby image");
        require(sha256_file(argv[3])=="35d381ffb06ff45f7357d3ef1634719bcfd4d5810eba1de9b32ca0443b62f538","Unsupported local AOT library");
        std::ifstream input(argv[2],std::ios::binary);std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
        Library module(argv[3]);Gate gate(Elf32Image::from_file(argv[1]),bytes,module);
        for(auto mirror:{0u,0x40000000u}) gate.check(mirror);
        std::ofstream out(argv[4]);require(bool(out),"Cannot create report");
        out<<"{\"schema_version\":1,\"scope\":\"original_lobby_owner_construction\",\"success\":true,\"calls_per_path\":"<<gate.calls
           <<",\"constructor_cases\":"<<gate.constructor_cases<<",\"state_reset_cases\":"<<gate.reset_cases
           <<",\"command_release_cases\":"<<gate.command_release_cases
           <<",\"freed_owner_vptr_preserved_cases\":"<<gate.stale_vptr_cases<<",\"owner_address_reuses\":"<<gate.address_reuses
           <<",\"max_interpreter_slices\":"<<gate.max_steps<<",\"max_aot_dispatches\":"<<gate.max_dispatches
           <<",\"full_ram_vram_cpu_compared\":true,\"resource_loader_executed\":false,\"executed_pages\":[";
        bool comma=false;for(auto pc:gate.executed_pages){if(comma)out<<',';out<<pc;comma=true;}out<<"]}\n";
        out.close();require(bool(out),"Report write failed");std::cout<<"Owner construction: "<<gate.calls<<" original calls per path; passed\n";return 0;
    } catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
