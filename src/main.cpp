#include <iostream>
#include <memory>
#include <vector>
#include <sstream>

#include <llvm/Analysis/TargetLibraryInfo.h>

#include <llvm/IR/AutoUpgrade.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>

#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>

#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/WithColor.h>

#include <llvm/Bitcode/BitcodeWriter.h>

#include <llvm/CodeGen/CommandFlags.inc>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/CodeGen/MachineFunctionPass.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>

static auto exit_on_error = llvm::ExitOnError{};

// static std::unique_ptr<llvm::ToolOutputFile> getOutputStream(std::string_view target_name, llvm::Triple::OSType os, std::string_view program_name) {

//     std::error_code error;
//     auto flag = llvm::sys::fs::OpenFlags::F_None;
//     auto out = std::make_unique<llvm::ToolOutputFile>("out.obj", error, flag);
//     if (error) {
//         llvm::WithColor::error() << error.message() << "\n";
//         return nullptr;
//     }
//     return out;
// }

int main(int argc, char* argv[]) {

    //! Initialize LLVM
    llvm::InitLLVM(argc, argv);

    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmPrinters();

    auto *Registry = llvm::PassRegistry::getPassRegistry();
    llvm::initializeCore(*Registry);
    llvm::initializeCodeGen(*Registry);
    llvm::initializeLoopStrengthReducePass(*Registry);
    llvm::initializeLowerIntrinsicsPass(*Registry);
    llvm::initializeEntryExitInstrumenterPass(*Registry);
    llvm::initializePostInlineEntryExitInstrumenterPass(*Registry);
    llvm::initializeUnreachableBlockElimLegacyPassPass(*Registry);
    llvm::initializeConstantHoistingLegacyPassPass(*Registry);
    llvm::initializeScalarOpts(*Registry);
    llvm::initializeVectorization(*Registry);
    llvm::initializeScalarizeMaskedMemIntrinPass(*Registry);
    llvm::initializeExpandReductionsPass(*Registry);
    llvm::initializeHardwareLoopsPass(*Registry);

    //! Initialize debugging passes.
    llvm::initializeScavengerTestPass(*Registry);

    exit_on_error.setBanner(std::string(argv[0]) + ": ");

    //! Build IR

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("top", *context);
    llvm::IRBuilder<> builder(*context);

    auto func_type = llvm::FunctionType::get(builder.getInt64Ty(), false);
    auto main_function = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "main", *module);
    auto entrypoint = llvm::BasicBlock::Create(*context, "entrypoint", main_function);

    builder.SetInsertPoint(entrypoint);

    auto hello_world = builder.CreateGlobalStringPtr("Hello World!\n", "hello_world");


    std::vector<llvm::Type*> puts_args {builder.getInt8Ty()->getPointerTo()};
    llvm::ArrayRef<llvm::Type*> args_ref {puts_args};

    auto puts_type = llvm::FunctionType::get(builder.getInt32Ty(), args_ref, false);
    auto puts_function = module->getOrInsertFunction("puts", puts_type);

    builder.CreateCall(puts_function, hello_world);
    builder.CreateRet(builder.getInt64(0));

    //! Output IR and IR BitCode

    std::error_code err;
    llvm::raw_fd_ostream ros_ll{"out.ll", err, llvm::sys::fs::OpenFlags::F_None};
    llvm::raw_fd_ostream ros_bc{"out.bc", err, llvm::sys::fs::OpenFlags::F_None};

    module->print(ros_ll, nullptr);

    llvm::WriteBitcodeToFile(*module, ros_bc);


    //! Generate machine code

    auto cpu_string = getCPUStr();
    auto features_string = getFeaturesStr();
    auto optimize_level = llvm::CodeGenOpt::None;

    llvm::Triple triple(module->getTargetTriple());
    if (triple.getTriple().empty()) {
        triple.setTriple(llvm::sys::getDefaultTargetTriple());
    }

    std::string error_string;
    const auto* target = llvm::TargetRegistry::lookupTarget(MArch, triple, error_string);
    if (!target) {
        llvm::WithColor(errs()) << error_string;
    }

    auto target_options = InitTargetOptionsFromCodeGenFlags();
    target_options.DisableIntegratedAS = 0;
    target_options.MCOptions.ShowMCEncoding = false;
    target_options.MCOptions.MCUseDwarfDirectory = false;
    target_options.MCOptions.AsmVerbose = true;
    target_options.MCOptions.PreserveAsmComments = true;
    auto reloc_model = getRelocModel();

    if (triple.isOSAIX() && reloc_model.hasValue() && *reloc_model != llvm::Reloc::PIC_) {
        llvm::WithColor::error(llvm::errs()) << "invalide relocation model, AIS is only supports PIC.\n";
        return 1;
    }

    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(
        triple.getTriple(),
        cpu_string,
        features_string,
        target_options,
        reloc_model,
        getCodeModel(),
        optimize_level
    ));

    assert(target_machine && "Could not allocate target machine.");
    assert(module && "should will be exit by didn't have module.");

    // patch float abi
    if (FloatABIForCalls != FloatABI::Default) {
        target_options.FloatABIType = FloatABIForCalls;
    }

    auto out_file = std::make_unique<llvm::ToolOutputFile>("out.obj", err, llvm::sys::fs::OpenFlags::F_None);
    if (!out_file) {
        return 1;
    }

    llvm::legacy::PassManager pass_manager;
    llvm::TargetLibraryInfoImpl tlti(llvm::Triple(module->getTargetTriple()));
    pass_manager.add(new llvm::TargetLibraryInfoWrapperPass(tlti));

    module->setDataLayout(target_machine->createDataLayout());

    llvm::UpgradeDebugInfo(*module);

    if (llvm::verifyModule(*module, &errs())) {
        llvm::WithColor::error(errs()) << "input module is broken.\n";
        return 1;
    }

    setFunctionAttributes(cpu_string, features_string, *module);

    if (RelaxAll.getNumOccurrences() > 0 && FileType != llvm::TargetMachine::CGFT_ObjectFile) {
        WithColor::warning(errs()) << "ignoring relax because file type is not object.";
    }

    //! Do generate Assembly
    {
        llvm::raw_pwrite_stream* os = &out_file->os();

        // llvm::SmallVector<char, 0> buffer;
        // std::unique_ptr<llvm::raw_svector_ostream> bos;
        // if (FileType != llvm::TargetMachine::CGFT_AssemblyFile && !out_file->os().supportsSeeking()) {
        //     bos = std::make_unique<llvm::raw_svector_ostream>(buffer);
        //     os = bos.get();
        // }

        auto &llvm_target_machine = static_cast<LLVMTargetMachine&>(*target_machine);
        // auto& target_pass_config = *llvm_target_machine.createPassConfig(pass_manager);
        // if (target_pass_config.hasLimitedCodeGenPipeline()) {
        //     WithColor::warning(errs()) << target_pass_config.getLimitedCodeGenPipelineReason(" & ") << "\n";
        //     return 1;
        // }
        auto machine_module_info = new llvm::MachineModuleInfo(&llvm_target_machine);

        // pass_manager.add(&target_pass_config);
        // target_pass_config.printAndVerify("");

        // target_pass_config.setInitialized();
        // pass_manager.add(llvm::createPrintMIRPass(*os));
        // pass_manager.add(llvm::createFreeMachineFunctionPass());
        FileType = llvm::TargetMachine::CGFT_ObjectFile;
        if (target_machine->addPassesToEmitFile(pass_manager, *os, nullptr, FileType, false, machine_module_info)) {
            WithColor::warning(errs()) << "target does not support generation of this files type \n.";
            return 1;
        }

        pass_manager.run(*module);


        // if (bos) {
        //     out_file->os() << buffer;
        // }
        out_file->keep();
        out_file->os().flush();

        //! Do link and generate ELF

        std::stringstream link_command;
        link_command << "cc " << "out.obj" << " -o out.o";
        system(link_command.str().c_str());
    }



    return 0;
}

