#include "CodeCompiler.h"
#include <SFML/System.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <wx/filefn.h>
#include "GDL/CommonTools.h"
#include "GDL/Scene.h"

//Long list of llvm and clang headers
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/DiagnosticOptions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/Config/config.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TypeBuilder.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/GlobalVariable.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/system_error.h"
#include "llvm/Target/TargetRegistry.h"
#include "llvm/Target/TargetSelect.h"
#include "clang/Driver/Arg.h"
#include "clang/Driver/ArgList.h"
#include "clang/Driver/CC1Options.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/OptTable.h"
#include "clang/FrontendTool/Utils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "clang/Basic/FileManager.h"

using namespace std;
using namespace clang;
using namespace clang::driver;

CodeCompiler *CodeCompiler::_singleton = NULL;
sf::Mutex CodeCompiler::openSaveDialogMutex;

void CodeCompiler::ProcessTasks()
{
    while(true)
    {
        //Check if there is a task to be made
        {
            sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

            bool newTaskFound = false;
            for (unsigned int i = 0;i<pendingTasks.size();++i)
            {
                //Be sure that the task is not disabled
                if ( find(compilationDisallowed.begin(), compilationDisallowed.end(), pendingTasks[i].scene) == compilationDisallowed.end() )
                {
                    currentTask = pendingTasks[i];
                    pendingTasks.erase(pendingTasks.begin()+i);

                    newTaskFound = true;
                    break;
                }
            }
            if ( !newTaskFound ) //Bail out if no task can be made
            {
                if ( pendingTasks.empty() )
                    std::cout << "No more task to be processed." << std::endl;
                else
                    std::cout << "No more task to be processed ( But "+ToString(pendingTasks.size())+" disabled task(s) waiting for being enabled )." << std::endl;

                threadLaunched = false;
                return;
            }

        }

        std::cout << "Processing task..." << std::endl;

        if ( currentTask.preWork != boost::shared_ptr<CodeCompilerExtraWork>() )
        {
            std::cout << "Launching pre work" << std::endl;
            currentTask.preWork->Execute();
        }

        {
            //Define compilation arguments for Clang.
            llvm::SmallVector<const char *, 128> args;

            args.push_back(currentTask.inputFile.c_str());

            if ( currentTask.eventsGeneratedCode ) //Define special arguments for events generated code
            {
                #if defined(WINDOWS)
                if ( !currentTask.optimize ) //Don't use precompiled header when optimizing, as they are built without optimizations
                {
                    args.push_back("-include-pch");
                    args.push_back(!currentTask.compilationForRuntime ? "include/GDL/GDL/Events/PrecompiledHeader.h.pch" : "include/GDL/GDL/Events/PrecompiledHeaderRuntime.h.pch");
                }
                #endif

                args.push_back("-fsyntax-only");
                args.push_back("-fcxx-exceptions");
                args.push_back("-fexceptions");
                args.push_back("-fgnu-runtime");
                args.push_back("-fdeprecated-macro");
                args.push_back("-w"); //No warning
            }

            //Headers
            for (std::set<std::string>::const_iterator header = headersDirectories.begin();header != headersDirectories.end();++header)
                args.push_back((*header).c_str());

            if ( !currentTask.compilationForRuntime ) args.push_back("-DGD_IDE_ONLY"); //Already set in PCH
            if ( currentTask.optimize ) args.push_back("-O1");

            //GD library related defines. ( Also already set in PCH )
            #if defined(WINDOWS)
            args.push_back("-DGD_API=__declspec(dllimport)");
            args.push_back("-DGD_EXTENSION_API=__declspec(dllimport)");
            #elif defined(LINUX)
            args.push_back("-DGD_API= ");
            args.push_back("-DGD_EXTENSION_API= ");
            #elif defined(MAC)
            args.push_back("-DGD_API= ");
            args.push_back("-DGD_EXTENSION_API= ");
            #endif

            //Other common defines. ( Also already set in PCH )
            #if defined(RELEASE)
            args.push_back("-DRELEASE");
            args.push_back("-DNDEBUG");
            args.push_back("-DBOOST_DISABLE_ASSERTS");
            #elif defined(DEV)
            args.push_back("-DDEV");
            args.push_back("-DNDEBUG");
            args.push_back("-DBOOST_DISABLE_ASSERTS");
            #elif defined(DEBUG)
            args.push_back("-DDEBUG");
            #endif

            //The clang compiler instance
            std::cout << "Creating compiler instance...\n";
            CompilerInstance Clang;

            // Infer the builtin include path if unspecified.
            if (Clang.getHeaderSearchOpts().UseBuiltinIncludes && Clang.getHeaderSearchOpts().ResourceDir.empty())
            {
                Clang.getHeaderSearchOpts().ResourceDir = wxGetCwd();
                std::cout << "Set res dir to " << Clang.getHeaderSearchOpts().ResourceDir << std::endl;
            }

            //Diagnostic classes
            std::string compilationErrorFileErrors;
            llvm::raw_fd_ostream errorFile(std::string(workingDir+"compilationErrors.txt").c_str(), compilationErrorFileErrors);
            errorFile << "Please send this file to CompilGames@gmail.com, or include this content when reporting the problem to Game Develop's developer.\n";
            errorFile << "Veuillez envoyer ce fichier � CompilGames@gmail.com, ou l'inclure lorsque vous rapportez ce probl�me au d�veloppeur de Game Develop.\n";
            errorFile << "\n";
            errorFile << "Clang output:\n";
            if ( !compilationErrorFileErrors.empty() ) std::cout << "Unable to create compilation errors report file!\n";

            TextDiagnosticPrinter * clangDiagClient = new TextDiagnosticPrinter(errorFile, DiagnosticOptions());
            llvm::IntrusiveRefCntPtr<DiagnosticIDs> clangDiagID(new DiagnosticIDs());
            Diagnostic * clangDiags = new Diagnostic(clangDiagID, clangDiagClient);

            CompilerInvocation::CreateFromArgs(Clang.getInvocation(), args.begin(),  args.end(), *clangDiags);

            Clang.setDiagnostics(clangDiags);
            if (!Clang.hasDiagnostics())
            {
                std::cout << "Unable to create clang diagnostic engine!" << std::endl;
                return;
            }

            std::cout << "Compiling...\n";
            // Create and execute the frontend to generate an LLVM bitcode module.
            llvm::OwningPtr<CodeGenAction> Act(new EmitLLVMOnlyAction());
            if (!Clang.ExecuteAction(*Act))
            {
                std::cout << "Fatal error during compilation";
                return;
            }

            std::cout << "Writing bitcode...\n";
            sf::Lock lock(openSaveDialogMutex); //On windows, GD is crashing if we write bitcode while an open/save file dialog is displayed.

            llvm::OwningPtr<llvm::Module> module(Act->takeModule());

            std::string error;
            llvm::raw_fd_ostream file(currentTask.outputFile.c_str(), error, llvm::raw_fd_ostream::F_Binary);
            llvm::WriteBitcodeToFile(module.get(), file);
            std::cout << error;
        }

        if ( currentTask.postWork != boost::shared_ptr<CodeCompilerExtraWork>() )
        {
            std::cout << "Launching post task" << std::endl;
            currentTask.postWork->Execute();
        }

        std::cout << "Task ended." << std::endl;
    }
}

void CodeCompiler::AddTask(CodeCompilerTask task)
{
    {
        sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

        //Check if an equivalent task is not waiting in the pending list
        for (unsigned int i = 0;i<pendingTasks.size();++i)
        {
            if ( task.IsSameTaskAs(pendingTasks[i]) ) return;
        }

        pendingTasks.push_back(task);
        std::cout << "New task added.";
    }

    if ( !threadLaunched )
    {
        std::cout << "Launching compilation thread...";
        threadLaunched = true;
        currentTaskThread.Launch();
    }
}

std::vector < CodeCompilerTask > CodeCompiler::GetCurrentTasks() const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    std::vector < CodeCompilerTask > allTasks = pendingTasks;
    allTasks.insert(allTasks.begin(), currentTask);

    return allTasks;
}

bool CodeCompiler::HasTaskRelatedTo(Scene & scene) const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    if ( threadLaunched && currentTask.scene == &scene ) return true;

    for (unsigned int i = 0;i<pendingTasks.size();++i)
    {
        if ( pendingTasks[i].scene == &scene ) return true;
    }

    return false;
}

void CodeCompiler::EnableTaskRelatedTo(Scene & scene)
{
    bool mustLaunchCompilationThread = false;
    {
        sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

        std::cout << "Enabling tasks related to scene:" << scene.GetName() << endl;

        vector<Scene*>::iterator it = find(compilationDisallowed.begin(), compilationDisallowed.end(), &scene);
        if ( it != compilationDisallowed.end())
            compilationDisallowed.erase(it);

        mustLaunchCompilationThread = !pendingTasks.empty();
    }

    //Launch pending tasks if needed
    if ( !threadLaunched && mustLaunchCompilationThread )
    {
        std::cout << "Launching compilation thread...";
        threadLaunched = true;
        currentTaskThread.Launch();
    }
}

void CodeCompiler::RemovePendingTasksRelatedTo(Scene & scene)
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    for (unsigned int i = 0;i<pendingTasks.size();)
    {
        if ( pendingTasks[i].scene == &scene )
            pendingTasks.erase(pendingTasks.begin()+i);
        else
            ++i;
    }

}

void CodeCompiler::DisableTaskRelatedTo(Scene & scene)
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    std::cout << "Disabling tasks related to scene:" << scene.GetName() << endl;

    vector<Scene*>::iterator it = find(compilationDisallowed.begin(), compilationDisallowed.end(), &scene);
    if ( it == compilationDisallowed.end())
        compilationDisallowed.push_back(&scene);
}

bool CodeCompiler::CompilationInProcess() const
{
    sf::Lock lock(pendingTasksMutex); //Disallow modifying pending tasks.

    return (threadLaunched || !pendingTasks.empty());
}

void CodeCompiler::SetWorkingDirectory(std::string workingDir_)
{
    workingDir = workingDir_;
    if ( workingDir.empty() || (workingDir[workingDir.length()-1] != '/' && workingDir[workingDir.length()-1] != '\\' ) )
        workingDir += "/";

    if (!wxDirExists(workingDir.c_str()))
        wxMkdir(workingDir);
}

CodeCompiler::CodeCompiler() :
    threadLaunched(false),
    currentTaskThread(&CodeCompiler::ProcessTasks, this)
{
    #if defined(WINDOWS)
    headersDirectories.insert("-Iinclude/TDM-GCC-4.5.2/include");
    headersDirectories.insert("-Iinclude/TDM-GCC-4.5.2/lib/gcc/mingw32/4.5.2/include/c++");
    headersDirectories.insert("-Iinclude/TDM-GCC-4.5.2/lib/gcc/mingw32/4.5.2/include/c++/mingw32");
    #elif defined(LINUX)
    headersDirectories.insert("-Iinclude/linux/usr/include/i386-linux-gnu/");
    headersDirectories.insert("-Iinclude/linux/usr/include");
    headersDirectories.insert("-Iinclude/linux/usr/include/c++/4.6/");
    headersDirectories.insert("-Iinclude/linux/usr/include/c++/4.6/i686-linux-gnu");
    headersDirectories.insert("-Iinclude/linux/usr/include/c++/4.6/backward");
    #elif defined(MAC)

    #endif

    headersDirectories.insert("-Iinclude/llvm/tools/clang/lib/Headers");
    headersDirectories.insert("-Iinclude/GDL");
    headersDirectories.insert("-Iinclude/boost");
    headersDirectories.insert("-Iinclude/SFML/include");
    headersDirectories.insert("-Iinclude/wxwidgets/include");
    headersDirectories.insert("-Iinclude/wxwidgets/lib/gcc_dll/msw");
    headersDirectories.insert("-IExtensions/include");
}

CodeCompilerExtraWork::CodeCompilerExtraWork()
{
}
CodeCompilerExtraWork::~CodeCompilerExtraWork()
{
}

CodeCompiler::~CodeCompiler()
{
}

CodeCompiler * CodeCompiler::GetInstance()
{
    if ( NULL == _singleton )
        _singleton = new CodeCompiler;

    return ( static_cast<CodeCompiler*>( _singleton ) );
}

void CodeCompiler::DestroySingleton()
{
    if ( NULL != _singleton )
    {
        delete _singleton;
        _singleton = NULL;
    }
}
