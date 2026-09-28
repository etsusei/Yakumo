#include "native/bridge_contracts.hpp"
#include "native/texture_command_dispatch.hpp"
#include "native/texture_completion_progress.hpp"
#include "psprecomp/elf32.hpp"
#include "recomp_units.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t kManager=0x08200000u,kParam=0x08300000u,kFlags=0x08301000u;
constexpr std::uint32_t kDest=0x09000000u,kStack=0x08401000u,kEnd=0x08001000u;
constexpr std::uint32_t kWorker=0x08865450u,kChunk=32u;
void require(bool ok,const std::string &why){if(!ok)throw std::runtime_error(why);}
struct Import {std::uint32_t address,nid;const char *library;};
constexpr std::array<Import,11> kImports{{
    {0x08965928u,0xBFA98062u,"UtilsForUser"}, {0x089656A0u,0x6A638D83u,"IoFileMgrForUser"},
    {0x08965890u,0x68DA9E36u,"ThreadManForUser"}, {0x089658D0u,0x9ACE131Eu,"ThreadManForUser"},
    {0x08965908u,0x79D1C3FAu,"UtilsForUser"}, {0x08965840u,0x3AD58B8Cu,"ThreadManForUser"},
    {0x08965818u,0x27E22EC2u,"ThreadManForUser"}, {0x08965850u,0x3F53E640u,"ThreadManForUser"},
    {0x089658B0u,0x812346E4u,"ThreadManForUser"}, {0x089657D0u,0x1FB15A32u,"ThreadManForUser"},
    {0x08965858u,0x402FCF22u,"ThreadManForUser"}}};
struct Scenario {const char *name;std::vector<std::int32_t> results;bool injected_marker_clear{},group_cancel{},status_pointer{},normal_mode{},protected_dest{};std::uint32_t bytes{kChunk};bool hash_check{};std::vector<std::uint8_t> encoded{},decoded{};};
struct Boundary {
    Scenario scenario;std::uint32_t mirror{};std::size_t reads{},delays{},sleeps{},copies{};
    std::vector<std::array<std::uint32_t,5>> trace;
    std::function<void(std::uint32_t)> pump;
    std::vector<AllegrexContext> worker_contexts;
    bool in_worker{};unsigned worker_waits{},worker_acks{};
    mhp3rd::native::CompletionProgress completion_progress{};
    std::size_t completion_events{};
    bool completion_failed{};
    std::uint32_t completion_failed_checkpoint{};
    std::uint32_t completion_failed_stage{};
    std::vector<std::uint32_t> completion_trace;
    static void completion(void *data, const Runtime &, const AllegrexContext &ctx,
                           mhp3rd::native::TextureCompletionCheckpoint checkpoint,
                           std::uint32_t) noexcept {
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
        auto &self = *static_cast<Boundary *>(data);
        self.completion_trace.push_back(static_cast<std::uint32_t>(checkpoint));
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
        if (self.completion_progress.stage == mhp3rd::native::CompletionStage::Unsupported ||
            self.completion_progress.stage == mhp3rd::native::CompletionStage::Complete)
            return;
#endif
        using namespace mhp3rd::native;
        CompletionStep step{};
        bool no_op = false;
        switch (checkpoint) {
        case TextureCompletionCheckpoint::ClassifierReturn:
            if (ctx.gpr[2] != 0u) step = CompletionStep::UnsupportedCopyRoute;
            else {
                (void)self.completion_progress.accept(CompletionStep::InlineClassified);
                step = CompletionStep::CopyCall;
            }
            break;
        case TextureCompletionCheckpoint::CopyReturn: step = CompletionStep::CopyReturned; break;
        case TextureCompletionCheckpoint::HelperReturn: step = CompletionStep::HelperReturned; break;
        case TextureCompletionCheckpoint::PolicyReturn: step = CompletionStep::PolicyAccepted; break;
        case TextureCompletionCheckpoint::WorkerRequestCall: step = CompletionStep::WorkerRequestCall; break;
        case TextureCompletionCheckpoint::WorkerRequestReturn:
            step = CompletionStep::WorkerRequestReturned;
            break;
        case TextureCompletionCheckpoint::WorkerEventSetReturn:
            no_op = true;
            break;
        case TextureCompletionCheckpoint::WorkerWaitReturn:
            step = CompletionStep::MainWaitEntered;
            break;
        case TextureCompletionCheckpoint::WorkerEntry: step = CompletionStep::WorkerStarted; break;
        case TextureCompletionCheckpoint::VerbatimBranch: step = CompletionStep::VerbatimObserved; break;
        case TextureCompletionCheckpoint::DigestSkippedBranch:
            if (!self.completion_progress.needs_digest)
                step = CompletionStep::DigestSkipped;
            else
                no_op = true;
            break;
        case TextureCompletionCheckpoint::GroupCancellation:
        case TextureCompletionCheckpoint::FullQueueCancellation:
            no_op = true;
            break;
        case TextureCompletionCheckpoint::TransformCall: step = CompletionStep::TransformCall; break;
        case TextureCompletionCheckpoint::TransformReturn: step = CompletionStep::TransformReturned; break;
        case TextureCompletionCheckpoint::DigestCall: step = CompletionStep::DigestCall; break;
        case TextureCompletionCheckpoint::DigestReturn: step = CompletionStep::DigestReturned; break;
        case TextureCompletionCheckpoint::WorkerAckReturn:
            (void)self.completion_progress.accept(CompletionStep::WorkerAckCall);
            step = CompletionStep::WorkerAckReturned;
            break;
        case TextureCompletionCheckpoint::RetirementCall:
            (void)self.completion_progress.accept(CompletionStep::MainWaitReturned);
            step = CompletionStep::RetirementCall;
            break;
        case TextureCompletionCheckpoint::RetirementEntry: step = CompletionStep::RetirementEntered; break;
        case TextureCompletionCheckpoint::RetirementReturn: step = CompletionStep::RetirementReturned; break;
        case TextureCompletionCheckpoint::UnsupportedCopyRoute: step = CompletionStep::UnsupportedCopyRoute; break;
        case TextureCompletionCheckpoint::WorkerCopyReturn:
            no_op = true;
            break;
        }
        ++self.completion_events;
        if (!no_op && !self.completion_progress.accept(step)) {
            self.completion_failed = true;
            self.completion_failed_checkpoint = static_cast<std::uint32_t>(checkpoint);
            self.completion_failed_stage = static_cast<std::uint32_t>(self.completion_progress.stage);
        }
#else
        (void)data; (void)ctx; (void)checkpoint;
#endif
    }
    std::uint32_t address(std::uint32_t value)const{return value|mirror;}
    void invoke(std::uint32_t stub,Runtime &runtime,AllegrexContext &ctx){
        const auto args=std::array{ctx.gpr[4],ctx.gpr[5],ctx.gpr[6]};auto &m=runtime.memory();
        if(stub==0x089656A0u){
            require(reads<scenario.results.size(),"Unexpected extra read attempt");
            require(ctx.gpr[4]==7u && ctx.gpr[5]==address(kManager+0x98C0u) && ctx.gpr[6]==scenario.bytes,"Read arguments differ");
            const auto count=scenario.results[reads++];require(count<=static_cast<std::int32_t>(scenario.bytes),"Invalid fixture result");
            if(count>0){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
                for(std::size_t i=0;i<bytes.size();++i)bytes[i]=scenario.encoded.empty()?static_cast<std::uint8_t>(0x20u+i):scenario.encoded.at(i);
                m.copy_in(ctx.gpr[5],bytes);}
            ctx.gpr[2]=static_cast<std::uint32_t>(count);
        }else if(stub==0x08965890u){
            require(++delays<=4u && ctx.gpr[4]==10000u,"Unexpected delay/retry");
            if(scenario.injected_marker_clear)m.store16(address(kManager+0x8Cu),0u);
            ctx.gpr[2]=0u;
        }else if(stub==0x089658D0u){
            require(m.load16(address(kManager+0x8Cu))==0u,"Worker slept before request inactivity");
            ++sleeps;ctx.gpr[2]=0u;ctx.pc=kEnd;
        }else if(stub==0x08965858u){
            if(in_worker){
                require(ctx.gpr[5]==1u,"Unexpected worker event mask");
                if(++worker_waits>1u)ctx.pc=kEnd;
            }else{
                require(ctx.gpr[5]==16u && pump,"Unexpected main event wait");
                const auto id=ctx.gpr[4];require(id==11u||id==12u,"Unknown event worker");
                const auto before=worker_acks;pump(id==11u?0x088652C4u:0x08865378u);
                require(worker_acks==before+1u,"Original worker failed to acknowledge");
            }
            ctx.gpr[2]=0u;
        }else if(stub==0x089657D0u){
            if(in_worker&&ctx.gpr[5]==16u)++worker_acks;
            ctx.gpr[2]=0u;
        }else if(stub==0x08965928u){
            require(ctx.gpr[4]==address(kManager+0x98C0u) && ctx.gpr[5]==0x20000u,"Cache range differs");ctx.gpr[2]=0u;
        }else if(stub==0x08965908u){++copies;ctx.gpr[2]=0u;}
        else ctx.gpr[2]=0u;
        trace.push_back({stub,args[0],args[1],args[2],ctx.gpr[2]});
    }
};
class Gate {
    Runtime aot_,interpreted_;std::vector<std::uint8_t> baseline_;
    Boundary left_,right_;std::mt19937 random_{0x5452414Eu};
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
    mhp3rd::native::TextureCommandDispatch completion_aot_;
    mhp3rd::native::TextureCommandDispatch completion_interpreted_;
#endif
public:
    unsigned cases{},max_slices{},retry_cases{},marker_clears{},late_group_writes{},normal_cases{},transform_cases{},copy_worker_cases{},transform_worker_cases{},rounded_cases{},digest_cases{},digest_comparisons{};
    unsigned completion_event_cases{}, completion_failed_cases{};
    explicit Gate(const Elf32Image &elf):aot_(elf.required_ram_size()),interpreted_(elf.required_ram_size())
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
        , completion_aot_(aot_, mhp3rd::native::TextureCommandCallbacks{
            .user = &left_, .completion = &Boundary::completion}),
        completion_interpreted_(interpreted_, mhp3rd::native::TextureCommandCallbacks{
            .user = &right_, .completion = &Boundary::completion})
#endif
    {
        for(auto *r:{&aot_,&interpreted_})(void)elf.load_and_relocate(r->memory());
        register_generated_functions(aot_);baseline_=aot_.memory().bytes();
        for(const auto &imp:kImports)aot_.register_hle(imp.library,imp.nid,[this,imp](Runtime &r,AllegrexContext &c){left_.invoke(imp.address,r,c);});
    }
    void put(std::uint32_t address,std::uint32_t value){aot_.memory().store32(address,value);interpreted_.memory().store32(address,value);}
    void put8(std::uint32_t address,std::uint8_t value){aot_.memory().store8(address,value);interpreted_.memory().store8(address,value);}
    AllegrexContext context(std::uint32_t entry,std::uint32_t mirror){
        AllegrexContext c{};for(auto &v:c.gpr)v=random_();
        for(auto &v:c.fpr)v=std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        for(auto &v:c.vfpu)v=std::bit_cast<float>(static_cast<std::uint32_t>(random_()));
        c.gpr[0]=0;c.gpr[29]=kStack|mirror;c.gpr[31]=kEnd;c.pc=entry;return c;
    }
    AllegrexContext execute(Runtime &runtime,Boundary &boundary,AllegrexContext c,bool compiled){
        unsigned slices=0u;
        do{
            if(compiled){
                require(runtime.has_function(c.pc),"Missing AOT continuation");
                require(runtime.invoke_isolated_aot(c.pc,c)&&!runtime.stopped(),
                        "AOT stopped at "+std::to_string(c.pc)+": "+runtime.stop_reason());
            }else{
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
                switch (c.pc) {
                case 0x0886577Cu: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::ClassifierReturn, c.pc); break;
                case 0x088652ACu: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::CopyReturn, c.pc); break;
                case 0x088652C4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::UnsupportedCopyRoute, c.pc); break;
                case 0x088659B4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::HelperReturn, c.pc); break;
                case 0x088659C4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::PolicyReturn, c.pc); break;
                case 0x088659CCu: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerRequestCall, c.pc); break;
                case 0x088659E4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerRequestReturn, c.pc); break;
                case 0x088659F4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerEventSetReturn, c.pc); break;
                case 0x08865A10u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerWaitReturn, c.pc); break;
                case 0x08865378u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerEntry, c.pc); break;
                case 0x088653A0u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::VerbatimBranch, c.pc); break;
                case 0x088653B4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::DigestSkippedBranch, c.pc); break;
                case 0x08865420u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::TransformCall, c.pc); break;
                case 0x08865428u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::TransformReturn, c.pc); break;
                case 0x08865440u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::DigestCall, c.pc); break;
                case 0x08865448u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::DigestReturn, c.pc); break;
                case 0x088653C4u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::WorkerAckReturn, c.pc); break;
                case 0x08865814u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::RetirementCall, c.pc); break;
                case 0x08865D8Cu: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::RetirementEntry, c.pc); break;
                case 0x0886581Cu: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::RetirementReturn, c.pc); break;
                case 0x08866044u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::GroupCancellation, c.pc); break;
                case 0x08865F00u: Boundary::completion(&boundary, runtime, c,
                    mhp3rd::native::TextureCompletionCheckpoint::FullQueueCancellation, c.pc); break;
                default: break;
                }
#endif
                if(c.pc==0x08865CFCu)++digest_comparisons;
                const auto found=std::find_if(kImports.begin(),kImports.end(),[&](auto i){return i.address==c.pc;});
                if(found!=kImports.end()){
                    const auto pc=c.pc,return_pc=c.gpr[31];boundary.invoke(pc,runtime,c);
                    if(c.pc==pc)c.pc=return_pc;
                }else{
                    require((c.pc>=0x08863000u&&c.pc<0x08866200u)||
                        (c.pc>=0x0880BFF4u&&c.pc<0x0880E000u)||
                        (c.pc>=0x08819800u&&c.pc<0x0881A380u)||c.pc==0x088EE470u,
                        "Interpreter escaped worker/copy spans at "+std::to_string(c.pc));
                    require(interpret_allegrex(runtime,c,1u)==InterpreterExit::Budget&&!runtime.stopped(),"Interpreter stopped");
                }
            }
        }while(c.pc!=kEnd&&++slices<(compiled?10000u:2000000u));
        require(c.pc==kEnd,"Execution budget exhausted");if(!compiled)max_slices=std::max(max_slices,slices+1u);return c;
    }
    void pump(Runtime &runtime,Boundary &boundary,std::uint32_t entry,bool compiled){
        require(!boundary.in_worker,"Nested event worker unsupported");
        boundary.in_worker=true;boundary.worker_waits=0;
        AllegrexContext c{};c.pc=entry;c.gpr[4]=4u;c.gpr[5]=kParam|boundary.mirror;
        c.gpr[29]=(kStack-(entry==0x088652C4u?0x1000u:0x2000u))|boundary.mirror;c.gpr[31]=kEnd;
        boundary.worker_contexts.push_back(execute(runtime,boundary,c,compiled));
        require(boundary.worker_waits==2u,"Worker suspension boundary differs");boundary.in_worker=false;
    }
    void run(AllegrexContext c){
        const auto compiled=execute(aot_,left_,c,true),reference=execute(interpreted_,right_,c,false);
        require(mhp3rd::native::same_context(compiled,reference),"Worker CPU differs");
        require(aot_.memory().bytes()==interpreted_.memory().bytes(),"Worker RAM differs");
        require(aot_.memory().vram_bytes()==interpreted_.memory().vram_bytes(),"Worker VRAM differs");
        require(left_.trace==right_.trace,"Modeled import calls differ");
        require(left_.worker_contexts.size()==right_.worker_contexts.size(),"Secondary worker coverage differs");
        for(std::size_t i=0;i<left_.worker_contexts.size();++i)
            require(mhp3rd::native::same_context(left_.worker_contexts[i],right_.worker_contexts[i]),"Secondary worker CPU differs");
    }
    void check(const Scenario &s,std::uint32_t mirror){
        left_=Boundary{};left_.scenario=s;left_.mirror=mirror;
        left_.completion_progress = mhp3rd::native::CompletionProgress(s.normal_mode, s.hash_check);
        right_=left_;
        left_.pump=[this](auto entry){pump(aot_,left_,entry,true);};
        right_.pump=[this](auto entry){pump(interpreted_,right_,entry,false);};
        for(auto *r:{&aot_,&interpreted_}){r->memory().copy_in(GuestMemory::kPhysicalBase,baseline_);
            std::vector<std::uint8_t> guard(s.bytes+68u,0xA5u);r->memory().copy_in((s.protected_dest?0x08480000u:kDest)-32u,guard);}
        const auto destination=s.protected_dest?0x08480000u:kDest;
        const auto manager=kManager|mirror,request=manager+0x8Cu;
        put(kParam,manager);put(0x08AB3640u,kFlags);put(0x08A38644u,kFlags);
        put(manager+0x2F7F0u,11u);put(manager+0x2F7C8u,12u);
        put(manager+0x108Cu,0u);put(manager+0x1090u,1u);put(manager+0x1094u,8u);put(manager+0x1098u,7u);
        put(request,1u|((s.hash_check?1489u:17u)<<16u));put(request+4u,destination|mirror);put(request+8u,s.bytes);put(request+12u,0u);
        put(request+16u,s.bytes);put(request+20u,s.status_pointer?kFlags+0x400u:0u);
        put8(request+24u,5u);put8(request+25u,1u);put8(request+26u,1u);
        put8(request+28u,s.hash_check?1u:0u);
        if(s.hash_check)put(manager+0x3A40u+1489u*4u,285849u);
        put8(request+27u,s.normal_mode?1u:0u);put8(manager+0x2F7D4u,s.normal_mode?1u:0u);
        if(s.group_cancel){auto cancel=context(0x08866044u,mirror);cancel.gpr[4]=manager;cancel.gpr[5]=5u;run(cancel);
            require(aot_.memory().load16(request)==1u,"Group cancel removed active request");}
        auto worker=context(kWorker,mirror);worker.gpr[4]=4u;worker.gpr[5]=kParam|mirror;run(worker);
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
        const bool completion_match =
            left_.completion_events == right_.completion_events &&
            !left_.completion_failed && !right_.completion_failed;
        if (!completion_match) {
            // Missing or rejected completion callbacks are evidence of an
            // uncovered route. Keep the original execution result and record
            // the failure instead of synthesizing the missing event.
            ++completion_failed_cases;
        } else if (left_.completion_events != 0u) {
            ++completion_event_cases;
        }
#endif
        require(left_.reads==s.results.size()&&left_.sleeps==1u,"Read script or sleep coverage differs");
        require(left_.delays==(s.results.size()-1u+(s.injected_marker_clear?1u:0u)),"Retry coverage differs");
        const auto &m=aot_.memory();
        std::vector<std::uint8_t> expected(s.bytes+68u,0xA5u);
        if(!s.injected_marker_clear){
            for(std::uint32_t i=0;i<s.bytes;++i)expected[32u+i]=static_cast<std::uint8_t>(0x20u+i);
            if(s.hash_check){
                require(s.decoded.size()==s.bytes,"Decoded fixture extent differs");
                std::copy(s.decoded.begin(),s.decoded.end(),expected.begin()+32u);
                ++transform_cases;
            }else if(s.normal_mode){
                std::uint32_t high=0x2345u,low=0x7F8Du;
                for(std::uint32_t i=0;i<s.bytes;i+=4u){
                    high=(high*0x2345u)%0xFFD9u;low=(low*0x7F8Du)%0xFFF1u;
                    const auto key=(high<<16u)|low;
                    for(unsigned j=0;j<4u;++j)expected[32u+i+j]=
                        m.load8(0x089AF380u+expected[32u+i+j])^static_cast<std::uint8_t>(key>>(8u*j));
                }
                ++transform_cases;if(s.bytes%4u)++rounded_cases;
            }
        }
        for(std::size_t i=0;i<expected.size();++i)
            require(m.load8(destination-32u+static_cast<std::uint32_t>(i))==expected[i],"Destination extent or canary differs");
        const auto raw_dest=destination|mirror;
        const bool copy_worker=raw_dest>0x083FFFFFu && std::uint64_t(raw_dest)+s.bytes<=0x08800000u;
        require(left_.worker_contexts.size()==(s.injected_marker_clear?0u:(copy_worker?2u:1u)),"Actual event worker coverage differs");
        if(!s.injected_marker_clear){++transform_worker_cases;if(copy_worker)++copy_worker_cases;}
        if(s.normal_mode)++normal_cases;
        if(s.hash_check){
            for(unsigned i=0;i<20u;++i)require(m.load8(manager+0x2F754u+i)==m.load8(0x089AF480u+1489u*20u+i),
                "Original computed digest differs from original reference table");
            ++digest_cases;
        }
        require(left_.copies==(s.injected_marker_clear?0u:1u),"Copy completion coverage differs");
        require(m.load32(manager+0x298E0u)==(s.injected_marker_clear?0u:s.bytes),"Read byte accounting differs");
        if(!s.injected_marker_clear)require(m.load32(manager+0x108Cu)==1u&&m.load32(manager+0x1094u)==0u,"Retirement state differs");
        if(s.status_pointer&&s.group_cancel)require(m.load32(kFlags+0x400u)==1u,"Group status was not set");
        ++cases;if(s.results.size()>1u)++retry_cases;if(s.injected_marker_clear)++marker_clears;if(s.group_cancel)++late_group_writes;
    }
};
}
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
namespace mhp3rd::native {
// This oracle compares the complete original path through its terminal
// sleep import; only the compiled integration fixture stops at retirement.
bool texture_completion_oracle_stop_after_retirement(psprecomp::Runtime &,
    psprecomp::AllegrexContext &) noexcept { return false; }
}
#endif
int main(int argc,char **argv){try{
    require(argc==5 ||
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
            argc==7 ||
#endif
            false,
            "usage: texture_transfer_oracle EBOOT.ELF [overlay.bin module.dylib] new-report.json encoded-01489 decoded-01489");
    const char *report_path = argc == 7 ? argv[4] : argv[2];
    const char *encoded_path = argc == 7 ? argv[5] : argv[3];
    const char *decoded_path = argc == 7 ? argv[6] : argv[4];
    require(!std::filesystem::exists(report_path) && !std::filesystem::is_symlink(report_path),"Output exists");
    require(sha256_file(argv[1])=="55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c","Unsupported ELF");
    Gate gate(Elf32Image::from_file(argv[1]));
    const std::array<Scenario,7> scenarios{{{"full",{32}}, {"short_then_full",{3,32}},
        {"error_then_full",{-1,32}},{"zero_then_full",{0,32}},
        {"injected_marker_clear_after_short",{3},true}, {"active_group_cancel",{32},false,true},
        {"active_group_status_cancel",{32},false,true,true}}};
    for(auto s:scenarios)for(bool mode:{false,true})for(bool protected_dest:{false,true})for(auto mirror:{0u,0x40000000u}){
        s.normal_mode=mode;s.protected_dest=protected_dest;gate.check(s,mirror);
    }
    for(bool protected_dest:{false,true})for(auto mirror:{0u,0x40000000u}){
        Scenario s{"rounded_five_bytes",{5},false,false,false,true,protected_dest,5u};gate.check(s,mirror);
    }
    require(sha256_file(encoded_path)=="91f5655fe6f01631d64692c0be30e43acb90588e164e500770ec5dd41e972978" &&
            sha256_file(decoded_path)=="3f06d53ef775166b06a1bd98a8a03a0b79c5d895eb4ad652fc9ca3656a781885",
            "Private digest fixture identity differs");
    std::ifstream encoded_file(encoded_path,std::ios::binary),decoded_file(decoded_path,std::ios::binary);
    std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(encoded_file),{}},decoded{std::istreambuf_iterator<char>(decoded_file),{}};
    require(encoded.size()==22528u&&decoded.size()==22528u,"Private fixture size differs");
    for(bool protected_dest:{false,true})for(auto mirror:{0u,0x40000000u}){
        Scenario s{"original_digest_match",{22528},false,false,false,true,protected_dest,22528u,true,encoded,decoded};
        gate.check(s,mirror);
    }
    require(gate.digest_comparisons==0u,"State-eight route unexpectedly entered alternate-file digest comparison");
    std::ofstream out(report_path);require(bool(out),"Cannot create report");
    out<<"{\"schema_version\":1,\"scope\":\""
#ifdef MHP3RD_TEXTURE_COMPLETION_ORACLE
       <<"original_g1c_inline_completion_observation"
#else
       <<"original_read_copy_transform_with_modeled_imports"
#endif
       <<"\",\"success\":"<<(gate.completion_failed_cases == 0u ? "true" : "false")<<",\"cases\":"<<gate.cases
       <<",\"retry_cases\":"<<gate.retry_cases<<",\"marker_clear_cases\":"<<gate.marker_clears
       <<",\"late_group_write_cases\":"<<gate.late_group_writes<<",\"max_interpreter_slices\":"<<gate.max_slices
       <<",\"digest_match_cases\":"<<gate.digest_cases<<",\"digest_comparisons_observed\":"<<gate.digest_comparisons
       <<",\"normal_mode_cases\":"<<gate.normal_cases<<",\"data_transform_cases\":"<<gate.transform_cases
       <<",\"copy_worker_cases\":"<<gate.copy_worker_cases<<",\"transform_worker_cases\":"<<gate.transform_worker_cases
       <<",\"rounded_write_cases\":"<<gate.rounded_cases
       <<",\"full_ram_vram_cpu_compared\":true,\"imports_modeled\":true,\"event_scheduling_modeled\":true,"
       <<"\"transform_worker_executed\":true,\"completion_event_cases\":"<<gate.completion_event_cases
       <<",\"completion_failed_cases\":"<<gate.completion_failed_cases
       <<",\"game_executed\":false}\n";
    out.close();require(bool(out),"Report write failed");
    std::cout<<"Transfer gate: "<<gate.cases<<" scenarios per path; "
             <<(gate.completion_failed_cases == 0u ? "passed" : "completion coverage incomplete")<<'\n';
    return gate.completion_failed_cases == 0u ? 0 : 1;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
