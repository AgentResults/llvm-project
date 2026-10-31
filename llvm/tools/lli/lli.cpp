//===- lli.cpp - LLVM Interpreter / Dynamic compiler ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This utility provides a simple wrapper around the LLVM Execution Engines,
// which allow the direct execution of LLVM programs through a Just-In-Time
// compiler, or through an interpreter if no JIT is available for this platform.
//
//===----------------------------------------------------------------------===//

#include "ForwardingMemoryManager.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/DebugUtils.h"
#include "llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h"
#include "llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h"
#include "llvm/ExecutionEngine/Orc/EPCGenericRTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRPartitionLayer.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/SelfExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/SimpleRemoteEPC.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <cerrno>
#include <dlfcn.h>
#include <map>
#include <optional>
#include <set>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#else
#include <io.h>
#endif

#ifdef __CYGWIN__
#include <cygwin/version.h>
#if defined(CYGWIN_VERSION_DLL_MAJOR) && CYGWIN_VERSION_DLL_MAJOR<1007
#define DO_NOTHING_ATEXIT 1
#endif
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern "C" char **environ;
#endif

using namespace llvm;

static codegen::RegisterCodeGenFlags CGF;

#define DEBUG_TYPE "lli"

namespace {
enum class JITKind { MCJIT, Orc, OrcLazy };
enum class JITLinkerKind { Default, RuntimeDyld, JITLink };
} // namespace

static cl::opt<std::string> InputFile(cl::desc("<input bitcode>"),
                                      cl::Positional, cl::init("-"));

static cl::list<std::string> InputArgv(cl::ConsumeAfter,
                                       cl::desc("<program arguments>..."));

static cl::opt<bool>
    ForceInterpreter("force-interpreter",
                     cl::desc("Force interpretation: disable JIT"),
                     cl::init(false));

static cl::opt<JITKind>
    UseJITKind("jit-kind", cl::desc("Choose underlying JIT kind."),
               cl::init(JITKind::Orc),
               cl::values(clEnumValN(JITKind::MCJIT, "mcjit", "MCJIT"),
                          clEnumValN(JITKind::Orc, "orc", "Orc JIT"),
                          clEnumValN(JITKind::OrcLazy, "orc-lazy",
                                     "Orc-based lazy JIT.")));

static cl::opt<JITLinkerKind> JITLinker(
    "jit-linker", cl::desc("Choose the dynamic linker/loader."),
    cl::init(JITLinkerKind::Default),
    cl::values(clEnumValN(JITLinkerKind::Default, "default",
                          "Default for platform and JIT-kind"),
               clEnumValN(JITLinkerKind::RuntimeDyld, "rtdyld", "RuntimeDyld"),
               clEnumValN(JITLinkerKind::JITLink, "jitlink",
                          "Orc-specific linker")));
static cl::opt<std::string>
    OrcRuntime("orc-runtime", cl::desc("Use ORC runtime from given path"),
               cl::init(""));

static cl::opt<unsigned>
    LazyJITCompileThreads("compile-threads",
                          cl::desc("Choose the number of compile threads "
                                   "(jit-kind=orc-lazy only)"),
                          cl::init(0));

static cl::list<std::string>
    ThreadEntryPoints("thread-entry",
                      cl::desc("calls the given entry-point on a new thread "
                               "(jit-kind=orc-lazy only)"));

static cl::opt<bool> PerModuleLazy(
    "per-module-lazy",
    cl::desc("Performs lazy compilation on whole module boundaries "
             "rather than individual functions"),
    cl::init(false));

static cl::list<std::string>
    JITDylibs("jd",
              cl::desc("Specifies the JITDylib to be used for any subsequent "
                       "-extra-module arguments."));

static cl::list<std::string>
    Dylibs("dlopen", cl::desc("Dynamic libraries to load before linking"));

// The MCJIT supports building for a target address space separate from
// the JIT compilation process. Use a forked process and a copying
// memory manager with IPC to execute using this functionality.
static cl::opt<bool>
    RemoteMCJIT("remote-mcjit",
                cl::desc("Execute MCJIT'ed code in a separate process."),
                cl::init(false));

// Manually specify the child process for remote execution. This overrides
// the simulated remote execution that allocates address space for child
// execution. The child process will be executed and will communicate with
// lli via stdin/stdout pipes.
static cl::opt<std::string> ChildExecPath(
    "mcjit-remote-process",
    cl::desc("Specify the filename of the process to launch "
             "for remote MCJIT execution.  If none is specified,"
             "\n\tremote execution will be simulated in-process."),
    cl::value_desc("filename"), cl::init(""));

// Determine optimization level.
static cl::opt<char>
    OptLevel("O",
             cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                      "(default = '-O2')"),
             cl::Prefix, cl::init('2'));

static cl::opt<std::string>
    TargetTriple("mtriple", cl::desc("Override target triple for module"));

static cl::opt<std::string>
    EntryFunc("entry-function",
              cl::desc("Specify the entry function (default = 'main') "
                       "of the executable"),
              cl::value_desc("function"), cl::init("main"));

static cl::list<std::string>
    ExtraModules("extra-module", cl::desc("Extra modules to be loaded"),
                 cl::value_desc("input bitcode"));

static cl::list<std::string>
    ExtraObjects("extra-object", cl::desc("Extra object files to be loaded"),
                 cl::value_desc("input object"));

static cl::list<std::string>
    ExtraArchives("extra-archive", cl::desc("Extra archive files to be loaded"),
                  cl::value_desc("input archive"));

static cl::opt<bool>
    EnableCacheManager("enable-cache-manager",
                       cl::desc("Use cache manager to save/load modules"),
                       cl::init(false));

static cl::opt<std::string>
    ObjectCacheDir("object-cache-dir",
                   cl::desc("Directory to store cached object files "
                            "(must be user writable)"),
                   cl::init(""));

static cl::opt<std::string>
    FakeArgv0("fake-argv0",
              cl::desc("Override the 'argv[0]' value passed into the executing"
                       " program"),
              cl::value_desc("executable"));

static cl::opt<bool>
    DisableCoreFiles("disable-core-files", cl::Hidden,
                     cl::desc("Disable emission of core files if possible"));

static cl::opt<bool> NoLazyCompilation("disable-lazy-compilation",
                                       cl::desc("Disable JIT lazy compilation"),
                                       cl::init(false));

static cl::opt<bool> GenerateSoftFloatCalls(
    "soft-float", cl::desc("Generate software floating point library calls"),
    cl::init(false));

static cl::opt<bool> NoProcessSymbols(
    "no-process-syms",
    cl::desc("Do not resolve lli process symbols in JIT'd code"),
    cl::init(false));

enum class LLJITPlatform { Inactive, Auto, ExecutorNative, GenericIR };

static cl::opt<LLJITPlatform> Platform(
    "lljit-platform", cl::desc("Platform to use with LLJIT"),
    cl::init(LLJITPlatform::Auto),
    cl::values(clEnumValN(LLJITPlatform::Auto, "Auto",
                          "Like 'ExecutorNative' if ORC runtime "
                          "provided, otherwise like 'GenericIR'"),
               clEnumValN(LLJITPlatform::ExecutorNative, "ExecutorNative",
                          "Use the native platform for the executor."
                          "Requires -orc-runtime"),
               clEnumValN(LLJITPlatform::GenericIR, "GenericIR",
                          "Use LLJITGenericIRPlatform"),
               clEnumValN(LLJITPlatform::Inactive, "Inactive",
                          "Disable platform support explicitly")),
    cl::Hidden);

enum class DumpKind {
  NoDump,
  DumpFuncsToStdOut,
  DumpModsToStdOut,
  DumpModsToDisk,
  DumpDebugDescriptor,
  DumpDebugObjects,
};

static cl::opt<DumpKind> OrcDumpKind(
    "orc-lazy-debug", cl::desc("Debug dumping for the orc-lazy JIT."),
    cl::init(DumpKind::NoDump),
    cl::values(clEnumValN(DumpKind::NoDump, "no-dump", "Don't dump anything."),
               clEnumValN(DumpKind::DumpFuncsToStdOut, "funcs-to-stdout",
                          "Dump function names to stdout."),
               clEnumValN(DumpKind::DumpModsToStdOut, "mods-to-stdout",
                          "Dump modules to stdout."),
               clEnumValN(DumpKind::DumpModsToDisk, "mods-to-disk",
                          "Dump modules to the current "
                          "working directory. (WARNING: "
                          "will overwrite existing files)."),
               clEnumValN(DumpKind::DumpDebugDescriptor, "jit-debug-descriptor",
                          "Dump __jit_debug_descriptor contents to stdout"),
               clEnumValN(DumpKind::DumpDebugObjects, "jit-debug-objects",
                          "Dump __jit_debug_descriptor in-memory debug "
                          "objects as tool output")),
    cl::Hidden);

static ExitOnError ExitOnErr;

LLVM_ATTRIBUTE_USED static void linkComponents() {
  errs() << (void *)&llvm_orc_registerEHFrameSectionAllocAction
         << (void *)&llvm_orc_deregisterEHFrameSectionAllocAction
         << (void *)&llvm_orc_registerJITLoaderGDBAllocAction;
}

namespace {
//===----------------------------------------------------------------------===//
// Object cache
//
// This object cache implementation writes cached objects to disk to the
// directory specified by CacheDir, using a filename provided in the module
// descriptor. The cache tries to load a saved object using that path if the
// file exists. CacheDir defaults to "", in which case objects are cached
// alongside their originating bitcodes.
//
class LLIObjectCache : public ObjectCache {
public:
  LLIObjectCache(const std::string& CacheDir) : CacheDir(CacheDir) {
    // Add trailing '/' to cache dir if necessary.
    if (!this->CacheDir.empty() &&
        this->CacheDir[this->CacheDir.size() - 1] != '/')
      this->CacheDir += '/';
  }
  ~LLIObjectCache() override = default;

  void notifyObjectCompiled(const Module *M, MemoryBufferRef Obj) override {
    const std::string &ModuleID = M->getModuleIdentifier();
    std::string CacheName;
    if (!getCacheFilename(ModuleID, CacheName))
      return;
    if (!CacheDir.empty()) { // Create user-defined cache dir.
      SmallString<128> dir(sys::path::parent_path(CacheName));
      sys::fs::create_directories(Twine(dir));
    }

    std::error_code EC;
    raw_fd_ostream outfile(CacheName, EC, sys::fs::OF_None);
    outfile.write(Obj.getBufferStart(), Obj.getBufferSize());
    outfile.close();
  }

  std::unique_ptr<MemoryBuffer> getObject(const Module* M) override {
    const std::string &ModuleID = M->getModuleIdentifier();
    std::string CacheName;
    if (!getCacheFilename(ModuleID, CacheName))
      return nullptr;
    // Load the object from the cache filename
    ErrorOr<std::unique_ptr<MemoryBuffer>> IRObjectBuffer =
        MemoryBuffer::getFile(CacheName, /*IsText=*/false,
                              /*RequiresNullTerminator=*/false);
    // If the file isn't there, that's OK.
    if (!IRObjectBuffer)
      return nullptr;
    // MCJIT will want to write into this buffer, and we don't want that
    // because the file has probably just been mmapped.  Instead we make
    // a copy.  The filed-based buffer will be released when it goes
    // out of scope.
    return MemoryBuffer::getMemBufferCopy(IRObjectBuffer.get()->getBuffer());
  }

private:
  std::string CacheDir;

  bool getCacheFilename(StringRef ModID, std::string &CacheName) {
    if (!ModID.consume_front("file:"))
      return false;

    std::string CacheSubdir = std::string(ModID);
    // Transform "X:\foo" => "/X\foo" for convenience on Windows.
    if (is_style_windows(llvm::sys::path::Style::native) &&
        isalpha(CacheSubdir[0]) && CacheSubdir[1] == ':') {
      CacheSubdir[1] = CacheSubdir[0];
      CacheSubdir[0] = '/';
    }

    CacheName = CacheDir + CacheSubdir;
    size_t pos = CacheName.rfind('.');
    CacheName.replace(pos, CacheName.length() - pos, ".o");
    return true;
  }
};
} // namespace

// On Mingw and Cygwin, an external symbol named '__main' is called from the
// generated 'main' function to allow static initialization.  To avoid linking
// problems with remote targets (because lli's remote target support does not
// currently handle external linking) we add a secondary module which defines
// an empty '__main' function.
static void addCygMingExtraModule(ExecutionEngine &EE, LLVMContext &Context,
                                  const Triple &TargetTriple) {
  IRBuilder<> Builder(Context);

  // Create a new module.
  std::unique_ptr<Module> M = std::make_unique<Module>("CygMingHelper", Context);
  M->setTargetTriple(TargetTriple);

  // Create an empty function named "__main".
  Type *ReturnTy;
  if (TargetTriple.isArch64Bit())
    ReturnTy = Type::getInt64Ty(Context);
  else
    ReturnTy = Type::getInt32Ty(Context);
  Function *Result =
      Function::Create(FunctionType::get(ReturnTy, {}, false),
                       GlobalValue::ExternalLinkage, "__main", M.get());

  BasicBlock *BB = BasicBlock::Create(Context, "__main", Result);
  Builder.SetInsertPoint(BB);
  Value *ReturnVal = ConstantInt::get(ReturnTy, 0);
  Builder.CreateRet(ReturnVal);

  // Add this new module to the ExecutionEngine.
  EE.addModule(std::move(M));
}

static CodeGenOptLevel getOptLevel() {
  if (auto Level = CodeGenOpt::parseLevel(OptLevel))
    return *Level;
  WithColor::error(errs(), "lli") << "invalid optimization level.\n";
  exit(1);
}

[[noreturn]] static void reportError(SMDiagnostic Err, const char *ProgName) {
  Err.print(ProgName, errs());
  exit(1);
}

static Error loadDylibs();
static int runOrcJIT(const char *ProgName);
static void disallowOrcOptions();
static Expected<std::unique_ptr<orc::ExecutorProcessControl>> launchRemote();

//===----------------------------------------------------------------------===//
// main Driver function
//
// The tool's main as a callable function of a library (llvm-driver's `<tool>_main`), exported by liblli. The process
// environment is what the executable's `main` was handed as envp: a library is not handed it, so it reads it as the
// process does.
#if defined(__APPLE__)
static char *const *processEnvironment() { return *_NSGetEnviron(); }
#else
static char *const *processEnvironment() { return environ; }
#endif

LLVM_ABI_EXPORT int lli_main(int argc, char **argv, const llvm::ToolContext &) {
  char *const *envp = processEnvironment();
  InitLLVM X(argc, argv);

  if (argc > 1)
    ExitOnErr.setBanner(std::string(argv[0]) + ": ");

  // If we have a native target, initialize it to ensure it is linked in and
  // usable by the JIT.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  cl::ParseCommandLineOptions(argc, argv,
                              "llvm interpreter & dynamic compiler\n");

  // If the user doesn't want core files, disable them.
  if (DisableCoreFiles)
    sys::Process::PreventCoreFiles();

  ExitOnErr(loadDylibs());

  if (EntryFunc.empty()) {
    WithColor::error(errs(), argv[0])
        << "--entry-function name cannot be empty\n";
    exit(1);
  }

  if (UseJITKind == JITKind::MCJIT || ForceInterpreter)
    disallowOrcOptions();
  else
    return runOrcJIT(argv[0]);

  // Old lli implementation based on ExecutionEngine and MCJIT.
  LLVMContext Context;

  // Load the bitcode...
  SMDiagnostic Err;
  std::unique_ptr<Module> Owner = parseIRFile(InputFile, Err, Context);
  Module *Mod = Owner.get();
  if (!Mod)
    reportError(Err, argv[0]);

  if (EnableCacheManager) {
    std::string CacheName("file:");
    CacheName.append(InputFile);
    Mod->setModuleIdentifier(CacheName);
  }

  // If not jitting lazily, load the whole bitcode file eagerly too.
  if (NoLazyCompilation) {
    // Use *argv instead of argv[0] to work around a wrong GCC warning.
    ExitOnError ExitOnErr(std::string(*argv) +
                          ": bitcode didn't read correctly: ");
    ExitOnErr(Mod->materializeAll());
  }

  std::string ErrorMsg;
  EngineBuilder builder(std::move(Owner));
  builder.setMArch(codegen::getMArch());
  builder.setMCPU(codegen::getCPUStr());
  builder.setMAttrs(codegen::getFeatureList());
  if (auto RM = codegen::getExplicitRelocModel())
    builder.setRelocationModel(*RM);
  if (auto CM = codegen::getExplicitCodeModel())
    builder.setCodeModel(*CM);
  builder.setErrorStr(&ErrorMsg);
  builder.setEngineKind(ForceInterpreter
                        ? EngineKind::Interpreter
                        : EngineKind::JIT);

  // If we are supposed to override the target triple, do so now.
  if (!TargetTriple.empty())
    Mod->setTargetTriple(Triple(Triple::normalize(TargetTriple)));

  // Enable MCJIT if desired.
  RTDyldMemoryManager *RTDyldMM = nullptr;
  if (!ForceInterpreter) {
    if (RemoteMCJIT)
      RTDyldMM = new ForwardingMemoryManager();
    else
      RTDyldMM = new SectionMemoryManager();

    // Deliberately construct a temp std::unique_ptr to pass in. Do not null out
    // RTDyldMM: We still use it below, even though we don't own it.
    builder.setMCJITMemoryManager(
      std::unique_ptr<RTDyldMemoryManager>(RTDyldMM));
  } else if (RemoteMCJIT) {
    WithColor::error(errs(), argv[0])
        << "remote process execution does not work with the interpreter.\n";
    exit(1);
  }

  builder.setOptLevel(getOptLevel());

  TargetOptions Options =
      codegen::InitTargetOptionsFromCodeGenFlags(Triple(TargetTriple));
  if (codegen::getFloatABIForCalls() != FloatABI::Default)
    Options.FloatABIType = codegen::getFloatABIForCalls();

  builder.setTargetOptions(Options);

  std::unique_ptr<ExecutionEngine> EE(builder.create());
  if (!EE) {
    if (!ErrorMsg.empty())
      WithColor::error(errs(), argv[0])
          << "error creating EE: " << ErrorMsg << "\n";
    else
      WithColor::error(errs(), argv[0]) << "unknown error creating EE!\n";
    exit(1);
  }

  std::unique_ptr<LLIObjectCache> CacheManager;
  if (EnableCacheManager) {
    CacheManager.reset(new LLIObjectCache(ObjectCacheDir));
    EE->setObjectCache(CacheManager.get());
  }

  // Load any additional modules specified on the command line.
  for (unsigned i = 0, e = ExtraModules.size(); i != e; ++i) {
    std::unique_ptr<Module> XMod = parseIRFile(ExtraModules[i], Err, Context);
    if (!XMod)
      reportError(Err, argv[0]);
    if (EnableCacheManager) {
      std::string CacheName("file:");
      CacheName.append(ExtraModules[i]);
      XMod->setModuleIdentifier(CacheName);
    }
    EE->addModule(std::move(XMod));
  }

  for (unsigned i = 0, e = ExtraObjects.size(); i != e; ++i) {
    Expected<object::OwningBinary<object::ObjectFile>> Obj =
        object::ObjectFile::createObjectFile(ExtraObjects[i]);
    if (!Obj) {
      // TODO: Actually report errors helpfully.
      consumeError(Obj.takeError());
      reportError(Err, argv[0]);
    }
    object::OwningBinary<object::ObjectFile> &O = Obj.get();
    EE->addObjectFile(std::move(O));
  }

  for (unsigned i = 0, e = ExtraArchives.size(); i != e; ++i) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> ArBufOrErr =
        MemoryBuffer::getFileOrSTDIN(ExtraArchives[i]);
    if (!ArBufOrErr)
      reportError(Err, argv[0]);
    std::unique_ptr<MemoryBuffer> &ArBuf = ArBufOrErr.get();

    Expected<std::unique_ptr<object::Archive>> ArOrErr =
        object::Archive::create(ArBuf->getMemBufferRef());
    if (!ArOrErr) {
      std::string Buf;
      raw_string_ostream OS(Buf);
      logAllUnhandledErrors(ArOrErr.takeError(), OS);
      errs() << Buf;
      exit(1);
    }
    std::unique_ptr<object::Archive> &Ar = ArOrErr.get();

    object::OwningBinary<object::Archive> OB(std::move(Ar), std::move(ArBuf));

    EE->addArchive(std::move(OB));
  }

  // If the target is Cygwin/MingW and we are generating remote code, we
  // need an extra module to help out with linking.
  if (RemoteMCJIT && Mod->getTargetTriple().isOSCygMing()) {
    addCygMingExtraModule(*EE, Context, Mod->getTargetTriple());
  }

  // The following functions have no effect if their respective profiling
  // support wasn't enabled in the build configuration.
  EE->RegisterJITEventListener(
                JITEventListener::createOProfileJITEventListener());
  EE->RegisterJITEventListener(
                JITEventListener::createIntelJITEventListener());
  if (!RemoteMCJIT)
    EE->RegisterJITEventListener(
                JITEventListener::createPerfJITEventListener());

  if (!NoLazyCompilation && RemoteMCJIT) {
    WithColor::warning(errs(), argv[0])
        << "remote mcjit does not support lazy compilation\n";
    NoLazyCompilation = true;
  }
  EE->DisableLazyCompilation(NoLazyCompilation);

  // If the user specifically requested an argv[0] to pass into the program,
  // do it now.
  if (!FakeArgv0.empty()) {
    InputFile = static_cast<std::string>(FakeArgv0);
  } else {
    // Otherwise, if there is a .bc suffix on the executable strip it off, it
    // might confuse the program.
    if (StringRef(InputFile).ends_with(".bc"))
      InputFile.erase(InputFile.length() - 3);
  }

  // Add the module's name to the start of the vector of arguments to main().
  InputArgv.insert(InputArgv.begin(), InputFile);

  // Call the main function from M as if its signature were:
  //   int main (int argc, char **argv, const char **envp)
  // using the contents of Args to determine argc & argv, and the contents of
  // EnvVars to determine envp.
  //
  Function *EntryFn = Mod->getFunction(EntryFunc);
  if (!EntryFn) {
    WithColor::error(errs(), argv[0])
        << '\'' << EntryFunc << "\' function not found in module.\n";
    return -1;
  }

  // Reset errno to zero on entry to main.
  errno = 0;

  int Result = -1;

  // Sanity check use of remote-jit: LLI currently only supports use of the
  // remote JIT on Unix platforms.
  if (RemoteMCJIT) {
#ifndef LLVM_ON_UNIX
    WithColor::warning(errs(), argv[0])
        << "host does not support external remote targets.\n";
    WithColor::note() << "defaulting to local execution\n";
    return -1;
#else
    if (ChildExecPath.empty()) {
      WithColor::error(errs(), argv[0])
          << "-remote-mcjit requires -mcjit-remote-process.\n";
      exit(1);
    } else if (!sys::fs::can_execute(ChildExecPath)) {
      WithColor::error(errs(), argv[0])
          << "unable to find usable child executable: '" << ChildExecPath
          << "'\n";
      return -1;
    }
#endif
  }

  if (!RemoteMCJIT) {
    // If the program doesn't explicitly call exit, we will need the Exit
    // function later on to make an explicit call, so get the function now.
    FunctionCallee Exit = Mod->getOrInsertFunction(
        "exit", Type::getVoidTy(Context), Type::getInt32Ty(Context));

    // Run static constructors.
    if (!ForceInterpreter) {
      // Give MCJIT a chance to apply relocations and set page permissions.
      EE->finalizeObject();
    }
    EE->runStaticConstructorsDestructors(false);

    // Trigger compilation separately so code regions that need to be
    // invalidated will be known.
    (void)EE->getPointerToFunction(EntryFn);
    // Clear instruction cache before code will be executed.
    if (RTDyldMM)
      static_cast<SectionMemoryManager*>(RTDyldMM)->invalidateInstructionCache();

    // Run main.
    Result = EE->runFunctionAsMain(EntryFn, InputArgv, envp);

    // Run static destructors.
    EE->runStaticConstructorsDestructors(true);

    // If the program didn't call exit explicitly, we should call it now.
    // This ensures that any atexit handlers get called correctly.
    if (Function *ExitF =
            dyn_cast<Function>(Exit.getCallee()->stripPointerCasts())) {
      if (ExitF->getFunctionType() == Exit.getFunctionType()) {
        std::vector<GenericValue> Args;
        GenericValue ResultGV;
        ResultGV.IntVal = APInt(32, Result);
        Args.push_back(ResultGV);
        EE->runFunction(ExitF, Args);
        WithColor::error(errs(), argv[0])
            << "exit(" << Result << ") returned!\n";
        abort();
      }
    }
    WithColor::error(errs(), argv[0]) << "exit defined with wrong prototype!\n";
    abort();
  } else {
    // else == "if (RemoteMCJIT)"
    std::unique_ptr<orc::ExecutorProcessControl> EPC = ExitOnErr(launchRemote());

    // Remote target MCJIT doesn't (yet) support static constructors. No reason
    // it couldn't. This is a limitation of the LLI implementation, not the
    // MCJIT itself. FIXME.

    // Create a remote memory manager.
    auto RemoteMM = ExitOnErr(
        orc::EPCGenericRTDyldMemoryManager::CreateWithDefaultBootstrapSymbols(
            *EPC));

    // Forward MCJIT's memory manager calls to the remote memory manager.
    static_cast<ForwardingMemoryManager*>(RTDyldMM)->setMemMgr(
      std::move(RemoteMM));

    // Forward MCJIT's symbol resolution calls to the remote.
    static_cast<ForwardingMemoryManager *>(RTDyldMM)->setResolver(
        ExitOnErr(RemoteResolver::Create(*EPC)));
    // Grab the target address of the JIT'd main function on the remote and call
    // it.
    // FIXME: argv and envp handling.
    auto Entry =
        orc::ExecutorAddr(EE->getFunctionAddress(EntryFn->getName().str()));
    EE->finalizeObject();
    LLVM_DEBUG(dbgs() << "Executing '" << EntryFn->getName() << "' at 0x"
                      << format("%llx", Entry.getValue()) << "\n");
    Result = ExitOnErr(EPC->runAsMain(Entry, {}));

    // Like static constructors, the remote target MCJIT support doesn't handle
    // this yet. It could. FIXME.

    // Delete the EE - we need to tear it down *before* we terminate the session
    // with the remote, otherwise it'll crash when it tries to release resources
    // on a remote that has already been disconnected.
    EE.reset();

    // Signal the remote target that we're done JITing.
    ExitOnErr(EPC->disconnect());
  }

  return Result;
}

// JITLink debug support plugins put information about JITed code in this GDB
// JIT Interface global from OrcTargetProcess.
extern "C" LLVM_ABI struct jit_descriptor __jit_debug_descriptor;

static struct jit_code_entry *
findNextDebugDescriptorEntry(struct jit_code_entry *Latest) {
  if (Latest == nullptr)
    return __jit_debug_descriptor.first_entry;
  if (Latest->next_entry)
    return Latest->next_entry;
  return nullptr;
}

static ToolOutputFile &claimToolOutput() {
  static std::unique_ptr<ToolOutputFile> ToolOutput = nullptr;
  if (ToolOutput) {
    WithColor::error(errs(), "lli")
        << "Can not claim stdout for tool output twice\n";
    exit(1);
  }
  std::error_code EC;
  ToolOutput = std::make_unique<ToolOutputFile>("-", EC, sys::fs::OF_None);
  if (EC) {
    WithColor::error(errs(), "lli")
        << "Failed to create tool output file: " << EC.message() << "\n";
    exit(1);
  }
  return *ToolOutput;
}

static std::function<void(Module &)> createIRDebugDumper() {
  switch (OrcDumpKind) {
  case DumpKind::NoDump:
  case DumpKind::DumpDebugDescriptor:
  case DumpKind::DumpDebugObjects:
    return [](Module &M) {};

  case DumpKind::DumpFuncsToStdOut:
    return [](Module &M) {
      printf("[ ");

      for (const auto &F : M) {
        if (F.isDeclaration())
          continue;

        if (F.hasName()) {
          std::string Name(std::string(F.getName()));
          printf("%s ", Name.c_str());
        } else
          printf("<anon> ");
      }

      printf("]\n");
    };

  case DumpKind::DumpModsToStdOut:
    return [](Module &M) {
      outs() << "----- Module Start -----\n" << M << "----- Module End -----\n";
    };

  case DumpKind::DumpModsToDisk:
    return [](Module &M) {
      std::error_code EC;
      raw_fd_ostream Out(M.getModuleIdentifier() + ".ll", EC,
                         sys::fs::OF_TextWithCRLF);
      if (EC) {
        errs() << "Couldn't open " << M.getModuleIdentifier()
               << " for dumping.\nError:" << EC.message() << "\n";
        exit(1);
      }
      Out << M;
    };
  }
  llvm_unreachable("Unknown DumpKind");
}

static std::function<void(MemoryBuffer &)> createObjDebugDumper() {
  switch (OrcDumpKind) {
  case DumpKind::NoDump:
  case DumpKind::DumpFuncsToStdOut:
  case DumpKind::DumpModsToStdOut:
  case DumpKind::DumpModsToDisk:
    return [](MemoryBuffer &) {};

  case DumpKind::DumpDebugDescriptor: {
    // Dump the empty descriptor at startup once
    fprintf(stderr, "jit_debug_descriptor 0x%016" PRIx64 "\n",
            pointerToJITTargetAddress(__jit_debug_descriptor.first_entry));
    return [](MemoryBuffer &) {
      // Dump new entries as they appear
      static struct jit_code_entry *Latest = nullptr;
      while (auto *NewEntry = findNextDebugDescriptorEntry(Latest)) {
        fprintf(stderr, "jit_debug_descriptor 0x%016" PRIx64 "\n",
                pointerToJITTargetAddress(NewEntry));
        Latest = NewEntry;
      }
    };
  }

  case DumpKind::DumpDebugObjects: {
    return [](MemoryBuffer &Obj) {
      static struct jit_code_entry *Latest = nullptr;
      static ToolOutputFile &ToolOutput = claimToolOutput();
      while (auto *NewEntry = findNextDebugDescriptorEntry(Latest)) {
        ToolOutput.os().write(NewEntry->symfile_addr, NewEntry->symfile_size);
        Latest = NewEntry;
      }
    };
  }
  }
  llvm_unreachable("Unknown DumpKind");
}

static Error loadDylibs() {
  for (const auto &Dylib : Dylibs) {
    std::string ErrMsg;
    if (sys::DynamicLibrary::LoadLibraryPermanently(Dylib.c_str(), &ErrMsg))
      return make_error<StringError>(ErrMsg, inconvertibleErrorCode());
  }

  return Error::success();
}

/// velite: one name a library gives. Strong and Weak are DEFINITIONS, each by the library that defines it (the loaded image's
/// name); Reexport is no definition: the library only passes the name on from another.
struct ExportEntry {
  enum KindT { Strong, Weak, Reexport } Kind;
  std::string Name;
  std::string Definer;
};
using ExportList = std::vector<ExportEntry>;

#if defined(__APPLE__)
/// velite: THE INTERFACE OF A LIBRARY IS READ FROM THE IMAGE LOADED IN THIS PROCESS, never from an SDK's copy of it. Every
/// --dlopen library is dlopen'ed into lli's process (a system framework out of dyld's shared cache included), and each loaded
/// image carries in memory the export trie dyld itself binds from (LC_DYLD_EXPORTS_TRIE, or LC_DYLD_INFO's export_off): its
/// __LINKEDIT is at slide + vmaddr - fileoff, for a shared-cache image as for any other (the way dlsym and fishhook read it).
/// The trie gives each name's flags - weak definition, re-export (the install name of the dylib it comes from, by ordinal),
/// stub and resolver - and the image's LC_REEXPORT_DYLIB libraries are part of its interface, as dyld searches them. A
/// library's definer is the image that defines the name, not an umbrella that passes it on.
extern "C" {
uint32_t _dyld_image_count(void);
const char *_dyld_get_image_name(uint32_t);
const void *_dyld_get_image_header(uint32_t);
intptr_t _dyld_get_image_vmaddr_slide(uint32_t);
}

namespace {
struct Definition {
  ExportEntry::KindT Kind;
  std::string Definer;
};
using Interface = std::map<std::string, std::vector<Definition>>;

struct LoadedImage {
  const MachO::mach_header_64 *Header;
  intptr_t Slide;
  std::string Path;
  std::string InstallName;
};

class LoadedImages {
public:
  LoadedImages() {
    for (uint32_t I = 0, N = _dyld_image_count(); I != N; ++I) {
      const auto *H = static_cast<const MachO::mach_header_64 *>(_dyld_get_image_header(I));
      if (!H || H->magic != MachO::MH_MAGIC_64)
        continue;
      LoadedImage Img{H, _dyld_get_image_vmaddr_slide(I), _dyld_get_image_name(I), std::string()};
      (void)forEachCommand(H, [&](const MachO::load_command *LC) {
        if (LC->cmd == MachO::LC_ID_DYLIB)
          Img.InstallName = dylibName(LC);
      });
      Images.push_back(std::move(Img));
    }
  }

  /// The loaded image dlopen resolves Path to: the one whose name is Path, else the one dlopen gives the same handle for.
  Expected<size_t> imageOf(StringRef Path) {
    for (size_t I = 0; I != Images.size(); ++I)
      if (Images[I].Path == Path)
        return I;
    std::string P = Path.str();
    void *Handle = dlopen(P.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!Handle)
      return make_error<StringError>("--dlopen " + Path + ": not a library loaded in this process (" + dlerror() + ")",
                                     inconvertibleErrorCode());
    std::optional<size_t> Found;
    for (size_t I = 0; I != Images.size() && !Found; ++I) {
      void *H = dlopen(Images[I].Path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
      if (H) {
        if (H == Handle)
          Found = I;
        dlclose(H);
      }
    }
    dlclose(Handle);
    if (!Found)
      return make_error<StringError>("--dlopen " + Path + ": loaded, but its image is none of dyld's images",
                                     inconvertibleErrorCode());
    return *Found;
  }

  /// The loaded image whose path dladdr (or dyld) names, if any.
  std::optional<size_t> imageByPath(StringRef Path) const {
    for (size_t I = 0; I != Images.size(); ++I)
      if (Images[I].Path == Path)
        return I;
    return std::nullopt;
  }

  /// The interposing tuples of image I (its __DATA,__interpose section: {replacement, replacee} pairs of rebased pointers), as
  /// dyld applies them to every image's binds but the interposer's own.
  std::vector<std::pair<void *, void *>> interposeOf(size_t I) const {
    std::vector<std::pair<void *, void *>> Out;
    const LoadedImage &Img = Images[I];
    (void)forEachCommand(Img.Header, [&](const MachO::load_command *LC) {
      if (LC->cmd != MachO::LC_SEGMENT_64)
        return;
      const auto *Seg = reinterpret_cast<const MachO::segment_command_64 *>(LC);
      const auto *Sec = reinterpret_cast<const MachO::section_64 *>(Seg + 1);
      for (uint32_t K = 0; K != Seg->nsects; ++K) {
        if (StringRef(Sec[K].sectname, strnlen(Sec[K].sectname, sizeof(Sec[K].sectname))) != "__interpose")
          continue;
        const auto *Tuples = reinterpret_cast<void *const *>(Sec[K].addr + Img.Slide);
        for (uint64_t T = 0; T + 1 < Sec[K].size / sizeof(void *); T += 2)
          Out.push_back({Tuples[T], Tuples[T + 1]});
      }
    });
    return Out;
  }

  /// The interface of image I: every name it gives - its own export trie and, as dyld searches them, those of its
  /// LC_REEXPORT_DYLIB libraries - with the images that DEFINE it. A name is resolved to its definers lazily, by name, with
  /// the (image, name) pairs on the way recorded: a cycle of re-exports (Foundation and CoreServices re-export each other)
  /// ends where it meets itself and never leaves a name resolved short.
  Expected<const Interface *> interfaceOf(size_t I) {
    auto Cached = Cache.find(I);
    if (Cached != Cache.end())
      return &Cached->second;
    std::set<size_t> Reached;
    if (Error E = reach(I, Reached))
      return std::move(E);
    std::set<std::string> Names;
    for (size_t R : Reached) {
      auto D = direct(R);
      if (!D)
        return D.takeError();
      for (const auto &KV : (*D)->Own)
        Names.insert(KV.first);
    }
    Interface Result;
    for (const std::string &Name : Names) {
      std::set<std::pair<size_t, std::string>> Visiting;
      auto Defs = definersOf(I, Name, Visiting);
      if (!Defs)
        return Defs.takeError();
      if (!Defs->empty())
        Result[Name] = std::move(*Defs);
    }
    return &(Cache[I] = std::move(Result));
  }

private:
  struct OwnEntry {
    ExportEntry::KindT Kind;
    size_t Target = 0;       // a re-export: the image it comes from
    std::string ImportName;  // ...and the name it has there
  };
  struct Direct {
    std::map<std::string, std::vector<OwnEntry>> Own;
    std::vector<size_t> Reexported; // the images its LC_REEXPORT_DYLIB commands name
  };

  std::vector<LoadedImage> Images;
  std::map<size_t, Direct> Directs;
  std::map<size_t, Interface> Cache;

  /// Walks the load commands, trusting nothing of them past the header's own sizeofcmds. Returns false when malformed.
  template <typename F> static bool forEachCommand(const MachO::mach_header_64 *H, F Fn) {
    const uint8_t *P = reinterpret_cast<const uint8_t *>(H + 1);
    const uint8_t *End = P + H->sizeofcmds;
    for (uint32_t C = 0; C != H->ncmds; ++C) {
      if (End - P < ptrdiff_t(sizeof(MachO::load_command)))
        return false;
      const auto *LC = reinterpret_cast<const MachO::load_command *>(P);
      if (LC->cmdsize < sizeof(MachO::load_command) || LC->cmdsize > size_t(End - P))
        return false;
      Fn(LC);
      P += LC->cmdsize;
    }
    return true;
  }

  static std::string dylibName(const MachO::load_command *LC) {
    const auto *D = reinterpret_cast<const MachO::dylib_command *>(LC);
    return std::string(reinterpret_cast<const char *>(LC) + D->dylib.name);
  }

  /// The loaded image of a dylib a load command names: by its install name, else by the name dyld loaded it under.
  Expected<size_t> imageNamed(const std::string &Name) {
    for (size_t I = 0; I != Images.size(); ++I)
      if (Images[I].InstallName == Name)
        return I;
    for (size_t I = 0; I != Images.size(); ++I)
      if (Images[I].Path == Name)
        return I;
    return make_error<StringError>("a dylib an image names, " + Name + ", is none of the images loaded in this process",
                                   inconvertibleErrorCode());
  }

  /// Image I and every image its LC_REEXPORT_DYLIB commands reach.
  Error reach(size_t I, std::set<size_t> &Reached) {
    if (!Reached.insert(I).second)
      return Error::success();
    auto D = direct(I);
    if (!D)
      return D.takeError();
    for (size_t R : (*D)->Reexported)
      if (Error E = reach(R, Reached))
        return E;
    return Error::success();
  }

  /// The images that define Name as image I gives it: its own definitions, the definitions of the image each of its
  /// re-export entries names, and those of the libraries it re-exports. A re-export no definer is found for stays a
  /// Reexport, which says only that the name can be reached through I.
  Expected<std::vector<Definition>> definersOf(size_t I, const std::string &Name,
                                               std::set<std::pair<size_t, std::string>> &Visiting) {
    std::vector<Definition> Out;
    if (!Visiting.insert({I, Name}).second)
      return Out;
    auto D = direct(I);
    if (!D)
      return D.takeError();
    auto Add = [&](const Definition &Def) {
      for (const Definition &O : Out)
        if (O.Definer == Def.Definer && O.Kind == Def.Kind)
          return;
      Out.push_back(Def);
    };
    bool Passed = false;
    auto Own = (*D)->Own.find(Name);
    if (Own != (*D)->Own.end())
      for (const OwnEntry &E : Own->second) {
        if (E.Kind != ExportEntry::Reexport) {
          Add({E.Kind, Images[I].Path});
          continue;
        }
        Passed = true;
        auto From = definersOf(E.Target, E.ImportName.empty() ? Name : E.ImportName, Visiting);
        if (!From)
          return From.takeError();
        for (const Definition &Def : *From)
          if (Def.Kind != ExportEntry::Reexport)
            Add(Def);
      }
    // dyld's lookup takes an image's own export first and searches the libraries it re-exports only when it has none
    if (!Out.empty())
      return Out;
    for (size_t R : (*D)->Reexported) {
      auto Sub = definersOf(R, Name, Visiting);
      if (!Sub)
        return Sub.takeError();
      for (const Definition &Def : *Sub)
        Add(Def);
    }
    if (Out.empty() && Passed)
      Out.push_back({ExportEntry::Reexport, std::string()});
    return Out;
  }

  /// Image I's own export trie, parsed once, and the images its commands name.
  Expected<const Direct *> direct(size_t I) {
    auto Cached = Directs.find(I);
    if (Cached != Directs.end())
      return &Cached->second;
    const LoadedImage &Img = Images[I];
    struct Dependent {
      std::string Name;
      bool Reexported;
    };
    std::vector<Dependent> Dependents;
    const MachO::segment_command_64 *Linkedit = nullptr;
    uint32_t TrieOff = 0, TrieSize = 0;
    bool HaveTrie = false;
    bool Sound = forEachCommand(Img.Header, [&](const MachO::load_command *LC) {
      switch (LC->cmd) {
      case MachO::LC_SEGMENT_64: {
        const auto *Seg = reinterpret_cast<const MachO::segment_command_64 *>(LC);
        if (StringRef(Seg->segname, strnlen(Seg->segname, sizeof(Seg->segname))) == "__LINKEDIT")
          Linkedit = Seg;
        break;
      }
      case MachO::LC_LOAD_DYLIB:
      case MachO::LC_LOAD_WEAK_DYLIB:
      case MachO::LC_LOAD_UPWARD_DYLIB:
      case MachO::LC_LAZY_LOAD_DYLIB:
        Dependents.push_back({dylibName(LC), false});
        break;
      case MachO::LC_REEXPORT_DYLIB:
        Dependents.push_back({dylibName(LC), true});
        break;
      case MachO::LC_DYLD_EXPORTS_TRIE: {
        const auto *T = reinterpret_cast<const MachO::linkedit_data_command *>(LC);
        TrieOff = T->dataoff;
        TrieSize = T->datasize;
        HaveTrie = true;
        break;
      }
      case MachO::LC_DYLD_INFO:
      case MachO::LC_DYLD_INFO_ONLY:
        if (!HaveTrie) {
          const auto *T = reinterpret_cast<const MachO::dyld_info_command *>(LC);
          TrieOff = T->export_off;
          TrieSize = T->export_size;
          HaveTrie = true;
        }
        break;
      }
    });
    if (!Sound)
      return make_error<StringError>(Img.Path + ": its load commands are malformed", inconvertibleErrorCode());
    Direct Result;
    for (const Dependent &D : Dependents) {
      if (!D.Reexported)
        continue;
      auto Sub = imageNamed(D.Name);
      if (!Sub)
        return Sub.takeError();
      Result.Reexported.push_back(*Sub);
    }
    if (HaveTrie && TrieSize) {
      if (!Linkedit || TrieOff < Linkedit->fileoff || uint64_t(TrieOff) + TrieSize > Linkedit->fileoff + Linkedit->filesize)
        return make_error<StringError>(Img.Path + ": its export trie is not inside its __LINKEDIT segment",
                                       inconvertibleErrorCode());
      const uint8_t *Base = reinterpret_cast<const uint8_t *>(Img.Slide + Linkedit->vmaddr - Linkedit->fileoff + TrieOff);
      Error Err = walkTrie(Base, Base + TrieSize, [&](const std::string &Name, uint64_t Flags, uint64_t Ordinal,
                                                        const std::string &ImportName) -> Error {
        OwnEntry E;
        if (Flags & MachO::EXPORT_SYMBOL_FLAGS_REEXPORT) {
          if (Ordinal == 0 || Ordinal > Dependents.size())
            return make_error<StringError>(Img.Path + ": re-exports " + Name + " from dylib ordinal " + Twine(Ordinal) +
                                               ", which it has not",
                                           inconvertibleErrorCode());
          auto From = imageNamed(Dependents[Ordinal - 1].Name);
          if (!From)
            return From.takeError();
          E.Kind = ExportEntry::Reexport;
          E.Target = *From;
          E.ImportName = ImportName;
        } else
          E.Kind = (Flags & MachO::EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION) ? ExportEntry::Weak : ExportEntry::Strong;
        Result.Own[Name].push_back(std::move(E));
        return Error::success();
      });
      if (Err)
        return std::move(Err);
    }
    return &(Directs[I] = std::move(Result));
  }

  static Expected<uint64_t> uleb(const uint8_t *&P, const uint8_t *End) {
    uint64_t V = 0;
    unsigned Shift = 0;
    for (;;) {
      if (P >= End || Shift > 63)
        return make_error<StringError>("a malformed export trie", inconvertibleErrorCode());
      uint8_t B = *P++;
      V |= uint64_t(B & 0x7f) << Shift;
      if (!(B & 0x80))
        return V;
      Shift += 7;
    }
  }

  static Expected<std::string> cstring(const uint8_t *&P, const uint8_t *End) {
    const uint8_t *S = P;
    while (P < End && *P)
      ++P;
    if (P >= End)
      return make_error<StringError>("a malformed export trie", inconvertibleErrorCode());
    std::string R(reinterpret_cast<const char *>(S), P - S);
    ++P;
    return R;
  }

  /// dyld's export trie (mach-o/loader.h, dyld_info_command): a node is a terminal size, the terminal info (flags, then the
  /// address, stub and resolver offsets, or for a re-export the dylib ordinal and the name it has there), a child count and
  /// the children (an edge label, the child's offset).
  template <typename F> static Error walkTrie(const uint8_t *Start, const uint8_t *End, F Emit) {
    struct Node {
      uint64_t Offset;
      std::string Prefix;
    };
    std::vector<Node> Stack = {{0, std::string()}};
    size_t Visited = 0;
    while (!Stack.empty()) {
      Node N = std::move(Stack.back());
      Stack.pop_back();
      if (N.Offset >= size_t(End - Start) || ++Visited > size_t(End - Start))
        return make_error<StringError>("a malformed export trie", inconvertibleErrorCode());
      const uint8_t *P = Start + N.Offset;
      auto Size = uleb(P, End);
      if (!Size)
        return Size.takeError();
      if (*Size > size_t(End - P))
        return make_error<StringError>("a malformed export trie", inconvertibleErrorCode());
      const uint8_t *After = P + *Size;
      if (*Size) {
        auto Flags = uleb(P, End);
        if (!Flags)
          return Flags.takeError();
        uint64_t Ordinal = 0;
        std::string ImportName;
        if (*Flags & MachO::EXPORT_SYMBOL_FLAGS_REEXPORT) {
          auto O = uleb(P, End);
          if (!O)
            return O.takeError();
          Ordinal = *O;
          auto Imp = cstring(P, End);
          if (!Imp)
            return Imp.takeError();
          ImportName = *Imp;
        }
        if (Error E = Emit(N.Prefix, *Flags, Ordinal, ImportName))
          return E;
      }
      P = After;
      if (P >= End)
        return make_error<StringError>("a malformed export trie", inconvertibleErrorCode());
      uint8_t Children = *P++;
      for (uint8_t C = 0; C != Children; ++C) {
        auto Edge = cstring(P, End);
        if (!Edge)
          return Edge.takeError();
        auto Off = uleb(P, End);
        if (!Off)
          return Off.takeError();
        Stack.push_back({*Off, N.Prefix + *Edge});
      }
    }
    return Error::success();
  }
};

/// The one table of images of this process: lli reads every interface of the link line, and asks dyld's own answer for a
/// weak name, through it.
LoadedImages &loadedImages() {
  static LoadedImages Images;
  return Images;
}
} // namespace

/// velite: the names an image of the line interposes, as dyld does (dyld interposing): each {replacement, replacee} tuple of its
/// __DATA,__interpose binds the replacee's name - found by the address dyld gave it - to the replacement in every image but its
/// own. A sanitizer's runtime replaces malloc, free and operator new this way, so that every image shares one heap.
static Expected<std::map<std::string, void *>> interposedBy(StringRef Path) {
  LoadedImages &Images = loadedImages();
  auto Image = Images.imageOf(Path);
  if (!Image)
    return Image.takeError();
  std::map<std::string, void *> Out;
  for (const auto &[Replacement, Replacee] : Images.interposeOf(*Image)) {
    Dl_info Info;
    if (Replacement == Replacee || !dladdr(Replacee, &Info) || !Info.dli_sname || Info.dli_saddr != Replacee)
      continue;
    Out["_" + std::string(Info.dli_sname)] = Replacement;
  }
  return Out;
}

/// velite: what a --dlopen library gives: the interface of the image loaded in this process.
static Expected<ExportList> exportsOf(const Triple &, StringRef Path) {
  LoadedImages &Images = loadedImages();
  auto Image = Images.imageOf(Path);
  if (!Image)
    return Image.takeError();
  auto Iface = Images.interfaceOf(*Image);
  if (!Iface)
    return Iface.takeError();
  ExportList Names;
  for (const auto &[Name, Defs] : **Iface)
    for (const Definition &D : Defs)
      Names.push_back({D.Kind, Name, D.Definer});
  return Names;
}
#else
/// velite: what an ELF --dlopen library gives: its defined global dynamic symbols, from its file. Any other file is refused,
/// naming it.
static Expected<ExportList> exportsOf(const Triple &, StringRef Path) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return createFileError(Path, Buf.getError());
  auto Bin = object::createBinary((*Buf)->getMemBufferRef());
  if (!Bin)
    return createFileError(Path, Bin.takeError());
  ExportList Names;
  if (auto *Elf = dyn_cast<object::ELFObjectFileBase>(Bin->get())) {
    for (const object::SymbolRef &S : Elf->getDynamicSymbolIterators()) {
      Expected<uint32_t> Flags = S.getFlags();
      if (!Flags)
        return createFileError(Path, Flags.takeError());
      if ((*Flags & object::SymbolRef::SF_Undefined) || !(*Flags & object::SymbolRef::SF_Global))
        continue;
      Expected<StringRef> Name = S.getName();
      if (!Name)
        return createFileError(Path, Name.takeError());
      Names.push_back({(*Flags & object::SymbolRef::SF_Weak) ? ExportEntry::Weak : ExportEntry::Strong, Name->str(),
                       Path.str()});
    }
    return Names;
  }
  return make_error<StringError>("--dlopen " + Path + ": not an ELF library, so the names it defines cannot be read",
                                 inconvertibleErrorCode());
}
#endif

/// velite: who gives a name. For each library of the link line that reaches it: Strong and Weak, the images that DEFINE it
/// (strongly, weakly) with the first library of the line that reaches each; Passed, the libraries that only pass it on from a
/// library whose definition is not found.
struct Provider {
  std::map<std::string, unsigned> Strong;
  std::map<std::string, unsigned> Weak;
  std::set<unsigned> Passed;
};

/// velite: what a name binds to: the library of the line whose handle gives its address, or the reason it binds to none.
struct Binding {
  std::optional<unsigned> Library;
  std::string Refusal;
};

/// velite: THE LINK LINE: the images dyld inserted into this process, the libraries the step gives, then the runtime's
/// (libSystem.B, libc++.1), and who gives each name. ONE table keyed by the DEFINING image serves the text, the ORC runtime
/// and every guard: the address of a name is read from the handle of the first library of the line that reaches its definer.
struct LinkLine {
  std::vector<std::string> Libraries;
  /// Libraries[0, FirstImplicit) are the libraries the step GIVES; the rest are the implicit system library - the process set
  /// (libSystem.B, libc++.1 and what they re-export), last on the line - as ld64 links libSystem implicitly after every
  /// library the link line names (ld64's implicit libSystem).
  unsigned FirstImplicit = 0;
  /// Names the images of the line interpose, with their replacements (dyld interposing).
  std::map<std::string, void *> Interposed;
  StringMap<Provider> Names;
  StringMap<Binding> Bound;

  void add(unsigned Owner, const ExportList &Exports) {
    for (const ExportEntry &E : Exports) {
      Provider &P = Names[E.Name];
      if (E.Kind == ExportEntry::Strong)
        P.Strong.emplace(E.Definer, Owner);
      else if (E.Kind == ExportEntry::Weak)
        P.Weak.emplace(E.Definer, Owner);
      else
        P.Passed.insert(Owner);
    }
  }

  /// dyld's own answer for a name: the image the process binds it to (dyld weak-def coalescing), by its path.
  static std::optional<std::string> dyldBinds(StringRef Name) {
#if defined(__APPLE__)
    if (Name.size() < 2)
      return std::nullopt;
    std::string Sym = Name.drop_front().str(); // the leading underscore of Mach-O
#else
    std::string Sym = Name.str();
#endif
    if (void *Addr = dlsym(RTLD_DEFAULT, Sym.c_str())) {
      Dl_info Info;
      if (dladdr(Addr, &Info) && Info.dli_fname)
        return std::string(Info.dli_fname);
    }
    return std::nullopt;
  }

  /// Computed once, when the line is complete. By the rule of dyld: the one image that defines a name strongly binds it (two
  /// are AmbiguousImport); with no strong definition the weak definitions coalesce to the one image dyld bound, which must be
  /// of the line - a name dyld bound outside it is refused, named, never given to the first library that has a copy.
  void bind() {
    for (auto &KV : Names) {
      const std::string Name = KV.first().str();
      const Provider &P = KV.second;
      Binding B;
      // The libraries the step gives bind before the implicit system library: as ld64 links libSystem implicitly, after every
      // library the link line names, so a name a given library defines is that library's and the process set answers only
      // the names no given library defines (ld64's implicit libSystem). Two GIVEN libraries defining a name strongly are
      // AmbiguousImport; a given library and the process set are not.
      std::vector<std::pair<std::string, unsigned>> StrongGiven, StrongImplicit;
      for (const auto &[Definer, Owner] : P.Strong)
        (Owner < FirstImplicit ? StrongGiven : StrongImplicit).push_back({Definer, Owner});
      auto Ambiguous = [&](const std::vector<std::pair<std::string, unsigned>> &Defs) {
        std::string Libs;
        for (const auto &[Definer, Owner] : Defs)
          Libs += (Libs.empty() ? "" : "; ") + Definer + " (given as " + Libraries[Owner] + ")";
        B.Refusal = "AmbiguousImport: " + Name + " is defined strongly by more than one library: " + Libs;
      };
      if (StrongGiven.size() > 1)
        Ambiguous(StrongGiven);
      else if (StrongGiven.size() == 1)
        B.Library = StrongGiven.front().second;
      else if (StrongImplicit.size() > 1)
        Ambiguous(StrongImplicit);
      else if (StrongImplicit.size() == 1)
        B.Library = StrongImplicit.front().second;
      else if (!P.Weak.empty()) {
        std::optional<std::string> Image = dyldBinds(Name);
        auto Canonical = Image ? P.Weak.find(*Image) : P.Weak.end();
        if (Canonical != P.Weak.end())
          B.Library = Canonical->second;
        else {
          std::string Libs;
          for (const auto &[Definer, Owner] : P.Weak)
            Libs += (Libs.empty() ? "" : ", ") + Definer;
          B.Refusal = "UnresolvedImport: " + Name + " is defined only weakly, by " + Libs + ", and dyld binds it to " +
                      (Image ? *Image + ", which is no library of the step's link line" : std::string("no image"));
        }
      } else if (P.Passed.size() == 1) {
        B.Library = *P.Passed.begin();
      } else {
        B.Refusal = "UnresolvedImport: " + Name + " is passed on by more than one library of the step and defined by none "
                    "that the line reads";
      }
      Bound[KV.first()] = std::move(B);
    }
  }

  bool servedBy(StringRef Name, unsigned Library) const {
    auto It = Bound.find(Name);
    return It != Bound.end() && It->second.Library == std::optional<unsigned>(Library);
  }
};

/// velite: refuses a lookup of a name the link line binds to none, with the reason: AmbiguousImport (two images define it
/// strongly, each named with the library the step gave it through), or a weak name dyld bound outside the line.
class LinkLineGuard : public orc::DefinitionGenerator {
public:
  LinkLineGuard(std::shared_ptr<LinkLine> Line) : Line(std::move(Line)) {}
  Error tryToGenerate(orc::LookupState &, orc::LookupKind, orc::JITDylib &, orc::JITDylibLookupFlags,
                      const orc::SymbolLookupSet &Symbols) override {
    for (const auto &KV : Symbols) {
      auto It = Line->Bound.find(*KV.first);
      if (It != Line->Bound.end() && !It->second.Refusal.empty())
        return make_error<StringError>(It->second.Refusal, inconvertibleErrorCode());
    }
    return Error::success();
  }

private:
  std::shared_ptr<LinkLine> Line;
};

/// velite: the last of every search: a name still unresolved when it is asked has no library that gives it - UnresolvedImport,
/// naming the symbol and the libraries checked. A weak reference stays unresolved (null), as a linker leaves it.
class UnresolvedImportGuard : public orc::DefinitionGenerator {
public:
  UnresolvedImportGuard(std::shared_ptr<LinkLine> Line) : Line(std::move(Line)) {}
  Error tryToGenerate(orc::LookupState &, orc::LookupKind, orc::JITDylib &, orc::JITDylibLookupFlags,
                      const orc::SymbolLookupSet &Symbols) override {
    for (const auto &KV : Symbols) {
      if (KV.second == orc::SymbolLookupFlags::WeaklyReferencedSymbol)
        continue;
      std::string Libs;
      for (const std::string &L : Line->Libraries)
        Libs += (Libs.empty() ? "" : ", ") + L;
      return make_error<StringError>("UnresolvedImport: " + (*KV.first).str() + ": no library the step gives exports it "
                                         "(checked: " + Libs + ")",
                                     inconvertibleErrorCode());
    }
    return Error::success();
  }

private:
  std::shared_ptr<LinkLine> Line;
};

/// velite: the link line as definitions of a JITDylib: the guard of refusals first, then lli's own entry points, then each
/// library's generator (its handle gives the address of every name the line binds to it), the UnresolvedImport guard last
/// (generators are tried in the order they were added: Core.cpp's CurDefGeneratorStack is the reverse of DefGenerators and is
/// popped from its back). lli's own entry points are its link, not the text's: the JIT debugger registration and the exception
/// frame registration are looked up by the plugins in this JITDylib, and are libLLVM's own functions.
static Error installLinkLine(orc::LLJIT &L, orc::JITDylib &JD, std::shared_ptr<LinkLine> Line, bool WithUnresolved) {
  JD.addGenerator(std::make_unique<LinkLineGuard>(Line));
  orc::SymbolMap Own;
  auto Entry = [&](const char *Name, void *Addr) {
    Own[L.mangleAndIntern(Name)] = {orc::ExecutorAddr::fromPtr(Addr), JITSymbolFlags::Exported};
  };
  Entry("llvm_orc_registerJITLoaderGDBAllocAction", reinterpret_cast<void *>(&llvm_orc_registerJITLoaderGDBAllocAction));
  Entry("llvm_orc_registerEHFrameSectionAllocAction",
        reinterpret_cast<void *>(&llvm_orc_registerEHFrameSectionAllocAction));
  Entry("llvm_orc_deregisterEHFrameSectionAllocAction",
        reinterpret_cast<void *>(&llvm_orc_deregisterEHFrameSectionAllocAction));
  // what the images of the line interpose binds to its replacement, as dyld binds every image but the interposer's own
  for (const auto &[Name, Replacement] : Line->Interposed)
    Own[L.getExecutionSession().intern(Name)] = {orc::ExecutorAddr::fromPtr(Replacement), JITSymbolFlags::Exported};
  if (auto Err = JD.define(orc::absoluteSymbols(std::move(Own))))
    return Err;
  for (unsigned I = 0; I != Line->Libraries.size(); ++I) {
    auto G = orc::EPCDynamicLibrarySearchGenerator::Load(
        L.getExecutionSession(), L.getDylibMgr(), Line->Libraries[I].c_str(),
        [Line, I](const orc::SymbolStringPtr &Name) { return Line->servedBy(*Name, I); });
    if (!G)
      return G.takeError();
    JD.addGenerator(std::move(*G));
  }
  // on a platform whose process symbols are searched after this JITDylib, what none of the line gives is left to them
  if (WithUnresolved)
    JD.addGenerator(std::make_unique<UnresolvedImportGuard>(Line));
  return Error::success();
}

static void exitOnLazyCallThroughFailure() { exit(1); }

static Expected<orc::ThreadSafeModule>
loadModule(StringRef Path, orc::ThreadSafeContext TSCtx) {
  SMDiagnostic Err;
  auto M = TSCtx.withContextDo(
      [&](LLVMContext *Ctx) { return parseIRFile(Path, Err, *Ctx); });
  if (!M) {
    std::string ErrMsg;
    {
      raw_string_ostream ErrMsgStream(ErrMsg);
      Err.print("lli", ErrMsgStream);
    }
    return make_error<StringError>(std::move(ErrMsg), inconvertibleErrorCode());
  }

  if (EnableCacheManager)
    M->setModuleIdentifier("file:" + M->getModuleIdentifier());

  return orc::ThreadSafeModule(std::move(M), std::move(TSCtx));
}

static int mingw_noop_main(void) {
  // Cygwin and MinGW insert calls from the main function to the runtime
  // function __main. The __main function is responsible for setting up main's
  // environment (e.g. running static constructors), however this is not needed
  // when running under lli: the executor process will have run non-JIT ctors,
  // and ORC will take care of running JIT'd ctors. To avoid a missing symbol
  // error we just implement __main as a no-op.
  //
  // FIXME: Move this to ORC-RT (and the ORC-RT substitution library once it
  //        exists). That will allow it to work out-of-process, and for all
  //        ORC tools (the problem isn't lli specific).
  return 0;
}

// Try to enable debugger support for the given instance.
// This alway returns success, but prints a warning if it's not able to enable
// debugger support.
static Error tryEnableDebugSupport(orc::LLJIT &J) {
  if (auto Err = enableDebuggerSupport(J)) {
    // a failure to enable the debugger's registration is said, never swallowed: a held text is found by it
    errs() << "lli: the JIT's debugger support is off: " << toString(std::move(Err)) << "\n";
  }
  return Error::success();
}

static int runOrcJIT(const char *ProgName) {
  // Start setting up the JIT environment.

  // Parse the main module.
  orc::ThreadSafeContext TSCtx(std::make_unique<LLVMContext>());
  auto MainModule = ExitOnErr(loadModule(InputFile, TSCtx));

  // Get TargetTriple and DataLayout from the main module if they're explicitly
  // set.
  std::optional<Triple> TT;
  std::optional<DataLayout> DL;
  MainModule.withModuleDo([&](Module &M) {
      if (!M.getTargetTriple().empty())
        TT = M.getTargetTriple();
      if (!M.getDataLayout().isDefault())
        DL = M.getDataLayout();
    });

  orc::LLLazyJITBuilder Builder;

  Builder.setJITTargetMachineBuilder(
      TT ? orc::JITTargetMachineBuilder(*TT)
         : ExitOnErr(orc::JITTargetMachineBuilder::detectHost()));

  TT = Builder.getJITTargetMachineBuilder()->getTargetTriple();
  if (DL)
    Builder.setDataLayout(DL);

  if (!codegen::getMArch().empty())
    Builder.getJITTargetMachineBuilder()->getTargetTriple().setArchName(
        codegen::getMArch());

  Builder.getJITTargetMachineBuilder()
      ->setCPU(codegen::getCPUStr())
      .addFeatures(codegen::getFeatureList())
      .setRelocationModel(codegen::getExplicitRelocModel())
      .setCodeModel(codegen::getExplicitCodeModel());

  // Link process symbols unless NoProcessSymbols is set.
  Builder.setLinkProcessSymbolsByDefault(!NoProcessSymbols);

  // FIXME: Setting a dummy call-through manager in non-lazy mode prevents the
  // JIT builder to instantiate a default (which would fail with an error for
  // unsupported architectures).
  if (UseJITKind != JITKind::OrcLazy) {
    auto ES = std::make_unique<orc::ExecutionSession>(
        ExitOnErr(orc::SelfExecutorProcessControl::Create()));
    Builder.setLazyCallthroughManager(
        std::make_unique<orc::LazyCallThroughManager>(*ES, orc::ExecutorAddr(),
                                                      nullptr));
    Builder.setExecutionSession(std::move(ES));
  }

  Builder.setLazyCompileFailureAddr(
      orc::ExecutorAddr::fromPtr(exitOnLazyCallThroughFailure));
  Builder.setNumCompileThreads(LazyJITCompileThreads);

  // If the object cache is enabled then set a custom compile function
  // creator to use the cache.
  std::unique_ptr<LLIObjectCache> CacheManager;
  if (EnableCacheManager) {

    CacheManager = std::make_unique<LLIObjectCache>(ObjectCacheDir);

    Builder.setCompileFunctionCreator(
      [&](orc::JITTargetMachineBuilder JTMB)
            -> Expected<std::unique_ptr<orc::IRCompileLayer::IRCompiler>> {
        if (LazyJITCompileThreads > 0)
          return std::make_unique<orc::ConcurrentIRCompiler>(std::move(JTMB),
                                                        CacheManager.get());

        auto TM = JTMB.createTargetMachine();
        if (!TM)
          return TM.takeError();

        return std::make_unique<orc::TMOwningSimpleCompiler>(std::move(*TM),
                                                        CacheManager.get());
      });
  }

  // Enable debugging of JIT'd code (only works on JITLink for ELF and MachO).
  Builder.setPrePlatformSetup(tryEnableDebugSupport);

  // Set up LLJIT platform.
  LLJITPlatform P = Platform;
  if (P == LLJITPlatform::Auto)
    P = OrcRuntime.empty() ? LLJITPlatform::GenericIR
                           : LLJITPlatform::ExecutorNative;

  switch (P) {
  case LLJITPlatform::ExecutorNative: {
    Builder.setPlatformSetUp(orc::ExecutorNativePlatform(OrcRuntime));
    break;
  }
  case LLJITPlatform::GenericIR:
    // Nothing to do: LLJITBuilder will use this by default.
    break;
  case LLJITPlatform::Inactive:
    Builder.setPlatformSetUp(orc::setUpInactivePlatform);
    break;
  default:
    llvm_unreachable("Unrecognized platform value");
  }

  switch (JITLinker) {
  case JITLinkerKind::JITLink:
    Builder.getJITTargetMachineBuilder()
        ->setRelocationModel(Reloc::PIC_)
        .setCodeModel(CodeModel::Small);
    Builder.setObjectLinkingLayerCreator(
        [&](orc::ExecutionSession &ES, jitlink::JITLinkMemoryManager &MemMgr) {
          return std::make_unique<orc::ObjectLinkingLayer>(ES, MemMgr);
        });
    break;
  case JITLinkerKind::RuntimeDyld:
    Builder.setObjectLinkingLayerCreator(
        [&](orc::ExecutionSession &ES, jitlink::JITLinkMemoryManager &MemMgr) {
          return std::make_unique<orc::RTDyldObjectLinkingLayer>(
              ES, [](const MemoryBuffer &) {
                return std::make_unique<SectionMemoryManager>();
              });
        });
    break;
  case JITLinkerKind::Default:
    // Let LLJITBuilder decide
    break;
  }

  // velite: THE --dlopen LIBRARIES ARE THE PROGRAM'S LINK LINE, AND NO ORDER DECIDES A BINDING. Each library has an interface
  // read from its image loaded in this process - the export trie dyld itself binds from - and the process's own runtime
  // (libSystem.B, libc++.1) is the implicit system library, last on the line, read the same way; the images of the line that
  // interpose (a sanitizer's runtime, given by the runner as the first library) bind their replaced names to their
  // replacements, as dyld does. ONE table keyed by the defining image serves the text, the ORC runtime and the guards, all as
  // the definitions of the process-symbols JITDylib the main and platform JITDylibs link: an undefined symbol binds to the
  // one image that defines it strongly (AmbiguousImport when two do); with no strong definition the weak definitions
  // coalesce to the one dyld bound (dyld weak-def coalescing); a name no library gives is UnresolvedImport. Every address
  // comes from its own library's handle. Searched through the process's global namespace, an import bound to whichever image
  // happened to define the name first - on macOS the system's LibreSSL libcrypto (mapped under CoreServices) outranked the
  // OpenSSL the program was given. The table is read before the JIT is created: the platform's bootstrap links the ORC
  // runtime against it.
  auto Line = std::make_shared<LinkLine>();
  const Triple &LinkTT = Builder.getJITTargetMachineBuilder()->getTargetTriple();
  const bool Darwin = LinkTT.isOSDarwin();
  if (Darwin && NoProcessSymbols)
    ExitOnErr(make_error<StringError>("-no-process-syms: on Darwin the process symbols ARE the step's link line (the "
                                      "libraries given and the runtime's), which the text and the ORC runtime link",
                                      inconvertibleErrorCode()));
  {
    std::vector<std::string> LineLibs;
    auto Put = [&](StringRef L) {
      if (!L.empty() && !is_contained(LineLibs, L))
        LineLibs.push_back(L.str());
    };
    // the libraries the step gives, in the order it gives them (the sanitizer runtime of a sanitized step first: the runner gives it)
    for (const auto &Dylib : Dylibs)
      if (!Darwin || (Dylib != "/usr/lib/libSystem.B.dylib" && Dylib != "/usr/lib/libc++.1.dylib"))
        Put(Dylib);
    Line->FirstImplicit = LineLibs.size();
    // the implicit system library, last: the process set
    if (Darwin) {
      Put("/usr/lib/libSystem.B.dylib");
      Put("/usr/lib/libc++.1.dylib");
    }
    for (const std::string &L : LineLibs) {
      Line->Libraries.push_back(L);
      Line->add(Line->Libraries.size() - 1, ExitOnErr(exportsOf(LinkTT, L)));
#if defined(__APPLE__)
      for (auto &KV : ExitOnErr(interposedBy(L)))
        Line->Interposed.emplace(KV.first, KV.second);
#endif
    }
    Line->bind();
  }
  if (Darwin)
    Builder.setProcessSymbolsJITDylibSetup([Line](orc::LLJIT &L) -> Expected<orc::JITDylibSP> {
      auto &PJD = L.getExecutionSession().createBareJITDylib("<Process Symbols>");
      if (auto Err = installLinkLine(L, PJD, Line, true))
        return std::move(Err);
      return &PJD;
    });

  auto J = ExitOnErr(Builder.create());

  // a platform that is not Darwin's keeps the process symbols it always had; the line's libraries are bound beside them
  if (!Darwin)
    ExitOnErr(installLinkLine(*J, J->getMainJITDylib(), Line, false));

  auto *ObjLayer = &J->getObjLinkingLayer();
  if (auto *RTDyldObjLayer = dyn_cast<orc::RTDyldObjectLinkingLayer>(ObjLayer)) {
    RTDyldObjLayer->registerJITEventListener(
        *JITEventListener::createGDBRegistrationListener());
#if LLVM_USE_OPROFILE
    RTDyldObjLayer->registerJITEventListener(
        *JITEventListener::createOProfileJITEventListener());
#endif
#if LLVM_USE_INTEL_JITEVENTS
    RTDyldObjLayer->registerJITEventListener(
        *JITEventListener::createIntelJITEventListener());
#endif
#if LLVM_USE_PERF
    RTDyldObjLayer->registerJITEventListener(
        *JITEventListener::createPerfJITEventListener());
#endif
  }

  if (PerModuleLazy)
    J->setPartitionFunction(orc::IRPartitionLayer::compileWholeModule);

  auto IRDump = createIRDebugDumper();
  J->getIRTransformLayer().setTransform(
      [&](orc::ThreadSafeModule TSM,
          const orc::MaterializationResponsibility &R) {
        TSM.withModuleDo([&](Module &M) {
          if (verifyModule(M, &dbgs())) {
            dbgs() << "Bad module: " << &M << "\n";
            exit(1);
          }
          IRDump(M);
        });
        return TSM;
      });

  auto ObjDump = createObjDebugDumper();
  J->getObjTransformLayer().setTransform(
      [&](std::unique_ptr<MemoryBuffer> Obj)
          -> Expected<std::unique_ptr<MemoryBuffer>> {
        ObjDump(*Obj);
        return std::move(Obj);
      });

  // If this is a Mingw or Cygwin executor then we need to alias __main to
  // orc_rt_int_void_return_0.
  if (J->getTargetTriple().isOSCygMing()) {
    auto &WorkaroundJD = J->getProcessSymbolsJITDylib()
                             ? *J->getProcessSymbolsJITDylib()
                             : J->getMainJITDylib();
    ExitOnErr(WorkaroundJD.define(
        orc::absoluteSymbols({{J->mangleAndIntern("__main"),
                               {orc::ExecutorAddr::fromPtr(mingw_noop_main),
                                JITSymbolFlags::Exported}}})));
  }

  // Regular modules are greedy: They materialize as a whole and trigger
  // materialization for all required symbols recursively. Lazy modules go
  // through partitioning and they replace outgoing calls with reexport stubs
  // that resolve on call-through.
  auto AddModule = [&](orc::JITDylib &JD, orc::ThreadSafeModule M) {
    return UseJITKind == JITKind::OrcLazy ? J->addLazyIRModule(JD, std::move(M))
                                          : J->addIRModule(JD, std::move(M));
  };

  // Add the main module.
  ExitOnErr(AddModule(J->getMainJITDylib(), std::move(MainModule)));

  // Create JITDylibs and add any extra modules.
  {
    // Create JITDylibs, keep a map from argument index to dylib. We will use
    // -extra-module argument indexes to determine what dylib to use for each
    // -extra-module.
    std::map<unsigned, orc::JITDylib *> IdxToDylib;
    IdxToDylib[0] = &J->getMainJITDylib();
    for (auto JDItr = JITDylibs.begin(), JDEnd = JITDylibs.end();
         JDItr != JDEnd; ++JDItr) {
      orc::JITDylib *JD = J->getJITDylibByName(*JDItr);
      if (!JD) {
        JD = &ExitOnErr(J->createJITDylib(*JDItr));
        J->getMainJITDylib().addToLinkOrder(*JD);
        JD->addToLinkOrder(J->getMainJITDylib());
      }
      IdxToDylib[JITDylibs.getPosition(JDItr - JITDylibs.begin())] = JD;
    }

    for (auto EMItr = ExtraModules.begin(), EMEnd = ExtraModules.end();
         EMItr != EMEnd; ++EMItr) {
      auto M = ExitOnErr(loadModule(*EMItr, TSCtx));

      auto EMIdx = ExtraModules.getPosition(EMItr - ExtraModules.begin());
      assert(EMIdx != 0 && "ExtraModule should have index > 0");
      auto JDItr = std::prev(IdxToDylib.lower_bound(EMIdx));
      auto &JD = *JDItr->second;
      ExitOnErr(AddModule(JD, std::move(M)));
    }

    for (auto EAItr = ExtraArchives.begin(), EAEnd = ExtraArchives.end();
         EAItr != EAEnd; ++EAItr) {
      auto EAIdx = ExtraArchives.getPosition(EAItr - ExtraArchives.begin());
      assert(EAIdx != 0 && "ExtraArchive should have index > 0");
      auto JDItr = std::prev(IdxToDylib.lower_bound(EAIdx));
      auto &JD = *JDItr->second;
      ExitOnErr(J->linkStaticLibraryInto(JD, EAItr->c_str()));
    }
  }

  // Add the objects.
  for (auto &ObjPath : ExtraObjects) {
    auto Obj = ExitOnErr(errorOrToExpected(MemoryBuffer::getFile(ObjPath)));
    ExitOnErr(J->addObjectFile(std::move(Obj)));
  }

  // Run any static constructors.
  ExitOnErr(J->initialize(J->getMainJITDylib()));

  // Run any -thread-entry points.
  std::vector<std::thread> AltEntryThreads;
  for (auto &ThreadEntryPoint : ThreadEntryPoints) {
    auto EntryPointSym = ExitOnErr(J->lookup(ThreadEntryPoint));
    typedef void (*EntryPointPtr)();
    auto EntryPoint = EntryPointSym.toPtr<EntryPointPtr>();
    AltEntryThreads.push_back(std::thread([EntryPoint]() { EntryPoint(); }));
  }

  // Resolve and run the main function.
  using MainFnTy = int(int, char *[]);
  auto MainAddr = ExitOnErr(J->lookup(EntryFunc));
  auto MainFn = MainAddr.toPtr<MainFnTy *>();
  int Result = orc::runAsMain(MainFn, InputArgv, StringRef(InputFile));

  // Wait for -entry-point threads.
  for (auto &AltEntryThread : AltEntryThreads)
    AltEntryThread.join();

  // Run destructors.
  ExitOnErr(J->deinitialize(J->getMainJITDylib()));

  return Result;
}

static void disallowOrcOptions() {
  // Make sure nobody used an orc-lazy specific option accidentally.

  if (LazyJITCompileThreads != 0) {
    errs() << "-compile-threads requires -jit-kind=orc-lazy\n";
    exit(1);
  }

  if (!ThreadEntryPoints.empty()) {
    errs() << "-thread-entry requires -jit-kind=orc-lazy\n";
    exit(1);
  }

  if (PerModuleLazy) {
    errs() << "-per-module-lazy requires -jit-kind=orc-lazy\n";
    exit(1);
  }
}

static Expected<std::unique_ptr<orc::ExecutorProcessControl>> launchRemote() {
#ifndef LLVM_ON_UNIX
  llvm_unreachable("launchRemote not supported on non-Unix platforms");
#else
  int PipeFD[2][2];
  pid_t ChildPID;

  // Create two pipes.
  if (pipe(PipeFD[0]) != 0 || pipe(PipeFD[1]) != 0)
    perror("Error creating pipe: ");

  ChildPID = fork();

  if (ChildPID == 0) {
    // In the child...

    // Close the parent ends of the pipes
    close(PipeFD[0][1]);
    close(PipeFD[1][0]);


    // Execute the child process.
    std::unique_ptr<char[]> ChildPath, ChildIn, ChildOut;
    {
      ChildPath.reset(new char[ChildExecPath.size() + 1]);
      llvm::copy(ChildExecPath, &ChildPath[0]);
      ChildPath[ChildExecPath.size()] = '\0';
      std::string ChildInStr = utostr(PipeFD[0][0]);
      ChildIn.reset(new char[ChildInStr.size() + 1]);
      llvm::copy(ChildInStr, &ChildIn[0]);
      ChildIn[ChildInStr.size()] = '\0';
      std::string ChildOutStr = utostr(PipeFD[1][1]);
      ChildOut.reset(new char[ChildOutStr.size() + 1]);
      llvm::copy(ChildOutStr, &ChildOut[0]);
      ChildOut[ChildOutStr.size()] = '\0';
    }

    char * const args[] = { &ChildPath[0], &ChildIn[0], &ChildOut[0], nullptr };
    int rc = execv(ChildExecPath.c_str(), args);
    if (rc != 0)
      perror("Error executing child process: ");
    llvm_unreachable("Error executing child process");
  }
  // else we're the parent...

  // Close the child ends of the pipes
  close(PipeFD[0][0]);
  close(PipeFD[1][1]);

  // Return a SimpleRemoteEPC instance connected to our end of the pipes.
  return orc::SimpleRemoteEPC::Create<orc::FDSimpleRemoteEPCTransport>(
      std::make_unique<llvm::orc::InPlaceTaskDispatcher>(), PipeFD[1][0],
      PipeFD[0][1]);
#endif
}

// For MinGW environments, manually export the __chkstk function from the lli
// executable.
//
// Normally, this function is provided by compiler-rt builtins or libgcc.
// It is named "_alloca" on i386, "___chkstk_ms" on x86_64, and "__chkstk" on
// arm/aarch64. In MSVC configurations, it's named "__chkstk" in all
// configurations.
//
// When Orc tries to resolve symbols at runtime, this succeeds in MSVC
// configurations, somewhat by accident/luck; kernelbase.dll does export a
// symbol named "__chkstk" which gets found by Orc, even if regular applications
// never link against that function from that DLL (it's linked in statically
// from a compiler support library).
//
// The MinGW specific symbol names aren't available in that DLL though.
// Therefore, manually export the relevant symbol from lli, to let it be
// found at runtime during tests.
//
// For real JIT uses, the real compiler support libraries should be linked
// in, somehow; this is a workaround to let tests pass.
//
// We need to make sure that this symbol actually is linked in when we
// try to export it; if no functions allocate a large enough stack area,
// nothing would reference it. Therefore, manually declare it and add a
// reference to it. (Note, the declarations of _alloca/___chkstk_ms/__chkstk
// are somewhat bogus, these functions use a different custom calling
// convention.)
//
// TODO: Move this into libORC at some point, see
// https://github.com/llvm/llvm-project/issues/56603.
#ifdef __MINGW32__
// This is a MinGW version of #pragma comment(linker, "...") that doesn't
// require compiling with -fms-extensions.
#if defined(__i386__)
#undef _alloca
extern "C" void _alloca(void);
static __attribute__((used)) void (*const ref_func)(void) = _alloca;
static __attribute__((section(".drectve"), used)) const char export_chkstk[] =
    "-export:_alloca";
#elif defined(__x86_64__)
extern "C" void ___chkstk_ms(void);
static __attribute__((used)) void (*const ref_func)(void) = ___chkstk_ms;
static __attribute__((section(".drectve"), used)) const char export_chkstk[] =
    "-export:___chkstk_ms";
#else
extern "C" void __chkstk(void);
static __attribute__((used)) void (*const ref_func)(void) = __chkstk;
static __attribute__((section(".drectve"), used)) const char export_chkstk[] =
    "-export:__chkstk";
#endif
#endif
