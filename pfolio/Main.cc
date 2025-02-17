/*****************************************************************************************[Main.cc]
 Glucose -- Copyright (c) 2009, Gilles Audemard, Laurent Simon
                CRIL - Univ. Artois, France
                LRI  - Univ. Paris Sud, France

Glucose sources are based on MiniSat (see below MiniSat copyrights). Permissions and copyrights of
Glucose are exactly the same as Minisat on which it is based on. (see below).

---------------

Copyright (c) 2003-2006, Niklas Een, Niklas Sorensson
Copyright (c) 2007-2010, Niklas Sorensson
Copyright (c) 2013-2016, Norbert Manthey, All rights reserved.

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/
#define _FPU_DOUBLE 1
#define _FPU_EXTENDED 0
#include <errno.h>

#include <signal.h>
#include <zlib.h>

#include "riss/utils/System.h"
#include "riss/utils/ParseUtils.h"
#include "riss/utils/Options.h"
#include "riss/core/Dimacs.h"
#include "pfolio/PSolver.h"

#include "coprocessor/Coprocessor.h"
#include <string>

#include "riss/utils/version.h" // include the file that defines the solver version

#include "riss/core/EnumerateMaster.h" // for model enumeration

using namespace Riss;
using namespace std;

//=================================================================================================


void printStats(PSolver& solver)
{
    double cpu_time = cpuTime();

    double mem_used = memUsedPeak();
    printf("c Memory used           : %.2f MB\n", mem_used);
    printf("c CPU time              : %g s\n", cpu_time);
}


static PSolver* solver;

static bool receivedInterupt = false;
// Terminate by notifying the solver and back out gracefully. This is mainly to have a test-case
// for this feature of the Solver as it may take longer than an immediate call to '_exit()'.
static void SIGINT_interrupt(int signum) { solver->interrupt(); }

// Note that '_exit()' rather than 'exit()' has to be used. The reason is that 'exit()' calls
// destructors and may cause deadlocks if a malloc/free function happens to be running (these
// functions are guarded by locks for multithreaded use).
static void SIGINT_exit(int signum)
{
    printf("\n"); printf("c *** INTERRUPTED ***\n");
//     if (solver->verbosity > 0){
//         printStats(*solver);
//         printf("\n"); printf("c *** INTERRUPTED ***\n"); }
    solver->interrupt();
    if (receivedInterupt) { _exit(1); }
    else { receivedInterupt = true; }
}


//=================================================================================================
// Main:


int main(int argc, char** argv)
{
    setUsageHelp("USAGE: %s [options] <input-file> <result-output-file>\n\n  where input may be either in plain or gzipped DIMACS.\n");
    // Extra options:
    //
    IntOption    verb("MAIN", "verb",   "Verbosity level (0=silent, 1=some, 2=more).", 1, IntRange(0, 2));
    IntOption    vv("MAIN", "vv",   "Verbosity every vv conflicts", 10000, IntRange(1, INT32_MAX));
    IntOption    cpu_lim("MAIN", "cpu-lim", "Limit on CPU time allowed in seconds.\n", INT32_MAX, IntRange(0, INT32_MAX));
    IntOption    mem_lim("MAIN", "mem-lim", "Limit on memory usage in megabytes.\n", INT32_MAX, IntRange(0, INT32_MAX));

    StringOption drupFile("PROOF", "proof", "Write a proof trace into the given file", 0);
    StringOption opt_proofFormat("PROOF", "proofFormat", "Do print the proof format (print o line with the given format, should be DRUP)", "DRUP");

    BoolOption   opt_checkModel("MAIN", "checkModel", "verify model inside the solver before printing (if input is a file)", false);
    BoolOption   opt_modelStyle("MAIN", "oldModel",   "present model on screen in old format", false);
    BoolOption   opt_quiet("MAIN", "quiet",      "Do not print the model", false);
    BoolOption   opt_parseOnly("MAIN", "parseOnly", "abort after parsing", false);
    StringOption opt_config("MAIN", "pconfig", "the configuration to be used for the portfolio solver", 0);
    BoolOption   opt_showParam("MAIN", "showUnusedParam", "print parameters after parsing", false);
    IntOption    opt_helpLevel("MAIN", "helpLevel", "Show only partial help.\n", -1, IntRange(-1, INT32_MAX));

    Int64Option  opt_enumeration("MODEL ENUMERATION", "models",     "number of models to be found (0=all)\n", -1, Int64Range(-1, INT64_MAX));
    IntOption    opt_enuMinimize("MODEL ENUMERATION", "modelMini",  "minimize blocking clause (0=no,1=from full,2=also from blocking)\n", 2, IntRange(0, 2));
    BoolOption   opt_enumPrintOFT("MODEL ENUMERATION", "enuOnline", "print model as soon as it has been found", true);
    Int64Option  opt_enumerationRec("MODEL ENUMERATION", "modelsRec",  "check every X decisions for new models\n", 512, Int64Range(1, INT64_MAX));
    IntOption    opt_recMinimize("MODEL ENUMERATION", "modelRMin",  "how to receive models(0=not,1=plain,2=mini full, 3=mini blocked)\n", 3, IntRange(0, 3));

    StringOption opt_projectionFile("MODEL ENUMERATION", "modelScope", "file that store enumeration projection\n", 0);
    StringOption opt_modelFile("MODEL ENUMERATION", "modelsFile", "file to store models to\n", 0);
    StringOption opt_fullModelFile("MODEL ENUMERATION", "fullModels", "file to store full models to\n", 0);
    StringOption opt_DNFfile("MODEL ENUMERATION", "dnf-file",   "file to store (reduced) DNF\n",  0);

    try {

        bool foundHelp = ::parseOptions(argc, argv);   // parse all global options
        PfolioConfig pfolioConfig(string(opt_config == 0 ? "" : opt_config));
        foundHelp = pfolioConfig.parseOptions(argc, argv, false, opt_helpLevel) || foundHelp;
        if (foundHelp) { exit(0); }  // stop after printing the help information

        if (opt_showParam) {  // print remaining parameters
            cerr << "c call after parsing options: ";
            for (int i = 0 ; i < argc; ++i) { cerr << " " << argv[i]; }
            cerr << endl;
        }

        PSolver S(&pfolioConfig);   // set up a portfolio solver for DRUP proofs

        double initial_time = cpuTime();

        S.verbosity = verb;
        S.verbEveryConflicts = vv;

        #if defined(__linux__)
        fpu_control_t oldcw, newcw;
        _FPU_GETCW(oldcw); newcw = (oldcw & ~_FPU_EXTENDED) | _FPU_DOUBLE; _FPU_SETCW(newcw);
        if (verb > 0) { printf("c WARNING: for repeatability, setting FPU to use double precision\n"); }
        #endif

        solver = &S;
        // Use signal handlers that forcibly quit until the solver will be able to respond to
        // interrupts:
        signal(SIGINT, SIGINT_exit);
        signal(SIGXCPU, SIGINT_exit);

        // Set limit on CPU-time:
        if (cpu_lim != INT32_MAX) {
            rlimit rl;
            getrlimit(RLIMIT_CPU, &rl);
            if (rl.rlim_max == RLIM_INFINITY || (rlim_t)cpu_lim < rl.rlim_max) {
                rl.rlim_cur = cpu_lim;
                if (setrlimit(RLIMIT_CPU, &rl) == -1) {
                    printf("c WARNING! Could not set resource limit: CPU-time.\n");
                }
            }
        }

        // Set limit on virtual memory:
        if (mem_lim != INT32_MAX) {
            rlim_t new_mem_lim = (rlim_t)mem_lim * 1024 * 1024;
            rlimit rl;
            getrlimit(RLIMIT_AS, &rl);
            if (rl.rlim_max == RLIM_INFINITY || new_mem_lim < rl.rlim_max) {
                rl.rlim_cur = new_mem_lim;
                if (setrlimit(RLIMIT_AS, &rl) == -1) {
                    printf("c WARNING! Could not set resource limit: Virtual memory.\n");
                }
            }
        }

        if (argc == 1) {
            printf("c Reading from standard input... Use '--help' for help.\n");
        }

        gzFile in = (argc == 1) ? gzdopen(0, "rb") : gzopen(argv[1], "rb");
        if (in == nullptr) {
            printf("c ERROR! Could not open file: %s\n", argc == 1 ? "<stdin>" : argv[1]), exit(1);
        }

        if (S.verbosity > 0) {
            printf("c =========================[ pfolio %s %13s ]===================================================\n", solverVersion, gitSHA1);
            printf("c | Norbert Manthey. The use of the tool is limited to research only!                                     |\n");
            printf("c | Based on Minisat 2.2 and Glucose 2.1  -- thanks!                                                      |\n");
            printf("c | Contributors:                                                                                         |\n");
            printf("c |      Kilian Gebhardt (BVE Implementation,parallel preprocessor)                                       |\n");
            printf("c ============================[ Problem Statistics ]=======================================================\n");
            printf("c |                                                                                                       |\n");
        }


        // open file for proof
        S.setDrupFile((drupFile) ? fopen((const char*) drupFile, "wb") : 0);
        if (opt_proofFormat && strlen(opt_proofFormat) > 0 && S.getDrupFile() != nullptr) { fprintf(S.getDrupFile(), "o proof %s\n", (const char*)opt_proofFormat); }     // we are writing proofs of the given format!

        parse_DIMACS(in, S);
        gzclose(in);
        FILE* res = (argc >= 3) ? fopen(argv[2], "wb") : nullptr;

        double parsed_time = cpuTime();
        if (S.verbosity > 0) {
            printf("c |  Number of variables:       %12d                                                              |\n", S.nVars());
            printf("c |  Number of clauses:         %12d                                                              |\n", S.nClauses());
            printf("c |  Number of total literals:  %12d                                                              |\n", S.nTotLits());
            printf("c |  Parse time:                %12.2f s                                                            |\n", parsed_time - initial_time);
            printf("c |                                                                                                       |\n");
        }

        if (opt_parseOnly) { exit(0); }   // simply stop here!

        // Change to signal-handlers that will only notify the solver and allow it to terminate
        // voluntarily:
        //signal(SIGINT, SIGINT_interrupt);
        //signal(SIGXCPU,SIGINT_interrupt);

        if (!S.simplify()) {
            if (res != nullptr) {
                if (opt_modelStyle) { fprintf(res, "UNSAT\n"), fclose(res); }
                else { fprintf(res, "s UNSATISFIABLE\n"), fclose(res); }
            }
            // add the empty clause to the proof, close proof file
            if (S.getDrupFile() != nullptr) { fprintf(S.getDrupFile(), "0\n"), fclose(S.getDrupFile()); }
            if (S.verbosity > 0) {
                printf("c =========================================================================================================\n");
                printf("c Solved by unit propagation\n");
                printStats(S);
            }

            // choose among output formats!
            if (opt_modelStyle) { printf("UNSAT"); }
            else { printf("s UNSATISFIABLE\n"); }
            cout.flush(); cerr.flush();
            exit(20);
        }


        Riss::EnumerateMaster* modelMaster = nullptr;
        if (opt_enumeration != -1) {
            modelMaster = new Riss::EnumerateMaster(S.nVars());
            modelMaster->setMaxModels(opt_enumeration);
            modelMaster->setModelMinimization((int) opt_enuMinimize);
            modelMaster->setShared(); // we are solving in parallel, hence, tell the enumeration object to synchronize with locks and to avoid duplicates eagerly!
            modelMaster->setPrintEagerly(opt_enumPrintOFT);

            modelMaster->setMinimizeReceived(opt_recMinimize == 0 ? 0 : opt_recMinimize - 1);
            modelMaster->setCheckEvery(opt_enumerationRec);
            modelMaster->setReceiveModels(opt_recMinimize != 0);

            if ((const char*) opt_projectionFile != 0) { modelMaster->setProjectionFile((const char*) opt_projectionFile); }
            if ((const char*) opt_modelFile != 0) { modelMaster->setModelFile((const char*) opt_modelFile); }
            if ((const char*) opt_fullModelFile != 0) { modelMaster->setFullModelFile((const char*) opt_fullModelFile); }
            if ((const char*) opt_DNFfile != 0) { modelMaster->setDNFfile((const char*) opt_DNFfile); }

            S.setEnumnerationMaster(modelMaster);   // finally, tell the portfolio solver about the enumeration master (parallel solver will finish setup)
        }

        vec<Lit> dummy;
        lbool ret = S.solveLimited(dummy);
        if (S.verbosity > 0) {
            printStats(S);
        }


        if (modelMaster != nullptr) {  // handle model enumeration
            if (S.verbosity > 0) { printf("c found models: %ld\n", modelMaster->foundModels()); }
            if (modelMaster->foundModels() > 0) {
                modelMaster->writeStreamToFile("", false); // for now, print all models to stderr is fine
                printf("s SATISFIABLE\n");
                if (res != nullptr) { fclose(res); res = nullptr; } // TODO: write result into output file!
                exit(30);
            }
        }

        // check model of the formula
        if (ret == l_True && opt_checkModel && argc != 1) {   // check the model if the formla was given via a file!
            gzFile in = (argc == 1) ? gzdopen(0, "rb") : gzopen(argv[1], "rb");
            if (in == nullptr) {
                printf("c ERROR! Could not open file: %s\n", argc == 1 ? "<stdin>" : argv[1]), exit(1);
            }
            if (check_DIMACS(in, S.model)) {
                printf("c verified model\n");
            } else {
                printf("c model invalid -- turn answer into UNKNOWN\n");
                ret = l_Undef; // turn result into unknown, because the model is not correct
            }
            gzclose(in);
        }

        // print solution to screen
        if (opt_modelStyle) { printf(ret == l_True ? "SAT\n" : ret == l_False ? "UNSAT\n" : "UNKNOWN\n"); }
        else { printf(ret == l_True ? "s SATISFIABLE\n" : ret == l_False ? "s UNSATISFIABLE\n" : "s UNKNOWN\n"); }

        // put empty clause on proof
        if (ret == l_False && S.getDrupFile() != nullptr) { fprintf(S.getDrupFile(), "0\n"); }

        // print solution into file
        if (res != nullptr) {
            if (ret == l_True) {
                if (opt_modelStyle) { fprintf(res, "SAT\n"); }
                else { fprintf(res, "s SATISFIABLE\nv "); }
                for (int i = 0; i < S.model.size(); i++)
                    //  if (S.model[i] != l_Undef) // treat undef simply as falsified (does not matter anyways)
                {
                    fprintf(res, "%s%s%d", (i == 0) ? "" : " ", (S.model[i] == l_True) ? "" : "-", i + 1);
                }
                fprintf(res, " 0\n");
            } else if (ret == l_False) {
                if (opt_modelStyle) { fprintf(res, "UNSAT\n"); }
                else { fprintf(res, "s UNSATISFIABLE\n"); }
            } else if (opt_modelStyle) { fprintf(res, "UNKNOWN\n"); }
            else { fprintf(res, "s UNKNOWN\n"); }
            fclose(res);
        }

        // print model to screen
        if (! opt_quiet && ret == l_True && res == nullptr) {
            if (!opt_modelStyle) { printf("v "); }
            for (int i = 0; i < S.model.size(); i++)
                //  if (S.model[i] != l_Undef) // treat undef simply as falsified (does not matter anyways)
            {
                printf("%s%s%d", (i == 0) ? "" : " ", (S.model[i] == l_True) ? "" : "-", i + 1);
            }
            printf(" 0\n");
        }

        cout.flush(); cerr.flush();

        #ifdef NDEBUG
        exit(ret == l_True ? 10 : ret == l_False ? 20 : 0);     // (faster than "return", which will invoke the destructor for 'Solver')
        #else
        return (ret == l_True ? 10 : ret == l_False ? 20 : 0);
        #endif
    } catch (OutOfMemoryException&) {
        // printf("c ===============================================================================\n");
        printf("c Warning: caught an exception\n");
        if (opt_modelStyle) { printf("UNKNOWN\n"); }
        else { printf("s UNKNOWN\n"); }
        exit(0);
    }
}
