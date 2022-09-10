/***
 * \file ecrash_test.c
 * 
 * This small program is used to test all of the functions of 
 * eCrash.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "eCrash.h"


/* Make our parameters global, so parseArguments can set some of the fields */
static eCrashParameters params;

/* Other globals set by arguments */
static int verbose = 0;
static int numThreads = 0;
static int secondsBeforeCrash = 3;
static int threadToCrash = 0;
static int unsafeBacktrace = 0;
static int useSymbolTable = 0;

typedef struct
{
    int threadNumber;
    int secondsBeforeCrash;
    int threadToCrash;
    int signo;
} eCrashTestParams;

/* Make a static symbol table */
int main(int argc, char *argv[]);
void *ecrash_test_thread(void *vparams);
int parseArguments(int argc, char *argv[]);
int CreateAThread(int i);
void BuildSymbolTable(eCrashSymbolTable *table);
void sleepFuncA(char *name);
void sleepFuncB(char *name);
void sleepFuncC(char *name);
void crashA(char *name);
void crashB(char *name);
void crashC(char *name);

eCrashSymbol symbols[] = {{"main",               (void *)main},
                          {"parseArguments",     (void *)parseArguments},
                          {"CreateAThread",      (void *)CreateAThread},
                          {"ecrash_test_thread", (void *)ecrash_test_thread},
                          {"BuildSymbolTable",   (void *)BuildSymbolTable},
                          {"sleepFuncA",         (void *)sleepFuncA},
                          {"sleepFuncB",         (void *)sleepFuncB},
                          {"sleepFuncC",         (void *)sleepFuncB},
                          {"crashA",             (void *)crashA},
                          {"crashB",             (void *)crashB},
                          {"crashC",             (void *)crashC},
};

/* some nested functions to make things prettier */
void sleepFuncC(char *name)
{
    printf("%s: Sleeping forever. . .\n", name);
    fflush(stdout);
    for (;;)
    {
        sleep(1);
    }
}

void sleepFuncB(char *name)
{
    sleepFuncC(name);
}

void sleepFuncA(char *name)
{
    sleepFuncB(name);
}


void crashC(char *name)
{
    int *kaBoom = NULL;
    printf("%s: kaBoom\n", name);
    fflush(stdout);
    *kaBoom = 7;
}

void crashB(char *name)
{
    crashC(name);
}

void crashA(char *name)
{
    crashB(name);
}

void *ecrash_test_thread(void *vparams)
{
    eCrashTestParams *params = (eCrashTestParams *)vparams;
    char threadName[256];

    /* Set up our name */
    sprintf(threadName, "Thread %d", params->threadNumber);

    /* Register for tracing */
    eCrash_RegisterThread(threadName, params->signo);

    /* Sleep & crash, or just sleep */
    if (params->threadToCrash == params->threadNumber)
    {
        printf("%s: Sleeping %d seconds before crash\n", threadName, params->secondsBeforeCrash);
        fflush(stdout);
        sleep(params->secondsBeforeCrash);
        crashA(threadName);
    }
    else
    {
        sleepFuncA(threadName);
    }

    return NULL;
}

int CreateAThread(int i)
{
    eCrashTestParams *parms;
    pthread_t thread;

    parms = malloc(sizeof(eCrashTestParams));

    parms->threadNumber = i;
    parms->secondsBeforeCrash = secondsBeforeCrash;
    parms->threadToCrash = threadToCrash + 1;
    parms->signo = 0; /* will force default */

    return pthread_create(&thread, NULL, ecrash_test_thread, (void *)parms);
}

int addressCompare(const void *va, const void *vb)
{
    eCrashSymbol *a = (eCrashSymbol *)va;
    eCrashSymbol *b = (eCrashSymbol *)vb;

    if (a->address < b->address)
    {
        return -1;
    }

    return 1;
}

void BuildSymbolTable(eCrashSymbolTable *table)
{
    /* First, build our table */
    table->numSymbols = sizeof(symbols) / sizeof(eCrashSymbol);
    table->symbols = symbols;

    /* Now, sort our symbol table by address */
    qsort(symbols, table->numSymbols, sizeof(eCrashSymbol), addressCompare);

}

/* Parse out our arguments */
#define USAGE "USAGE: %s [options]\n\
   Where options are one or more of:\n\
      -v,--verbose                     Be noisy.\n\
      -q,--quiet                       Be quiet.\n\
      -n,--num_threads <num>           Number of threads to spawn (0 default)\n\
      -s,--seconds_before_crash <num>  Seconds to wait before crashing\n\
      -t,--thread_to_crash <num>       Thread to crash (default = last one)\n\
      -x,--use_unsafe_backtrace        Use unsafe backtrace_symbols\n\
      -c,--use_symbol_table            Use safe custom symbol table.\n\
      -h,-?,--help                     This message\n\n"


int parseArguments(int argc, char *argv[])
{
    int c;

    while (1)
    {
        static struct option long_options[] = {
            /* These options set flags */
            {"verbose",              no_argument,       &verbose,         1},
            {"quiet",                no_argument,       &verbose,         0},
            {"use_unsafe_backtrace", no_argument,       &unsafeBacktrace, 1},
            {"use_symbol_table",     no_argument,       &useSymbolTable,  1},
            /* These options set values, so they have flags */
            {"num_threads",          required_argument, 0,                'n'},
            {"seconds_before_crash", required_argument, 0,                's'},
            {"thread_to_crash",      required_argument, 0,                't'},
            {"help",                 required_argument, 0,                'h'},
        };
        int option_index = 0;

        c = getopt_long(argc, argv, "cvqxn:s:t:h?", long_options, &option_index);
        if (c == -1)
        {
            break;
        }

        switch (c)
        {
        case 0: /* Was a flag setting option, maybe */
            if (long_options[option_index].flag != 0)
            {
                break;
            }
            printf("Strange option: %s\n", long_options[option_index].name);
            break;
        case 'n':
            numThreads = atol(optarg);
            break;
        case 's':
            secondsBeforeCrash = atol(optarg);
            break;
        case 't':
            threadToCrash = atol(optarg);
            break;
        case 'x':
            unsafeBacktrace = 1;
            break;
        case 'c':
            useSymbolTable = 1;
            break;
        case 'v':
            verbose = 1;
            break;
        case 'q':
            verbose = 0;
            break;
        case 'h':
        case '?':
            printf(USAGE, argv[0]);
            return 1;

        default:
            return -1;
        }
    }

    if (verbose)
    {
        printf("Arguments:\n");
        printf("             verbose: %s\n", verbose ? "yes" : "no");
        printf("          numThreads: %d\n", numThreads);
        printf("  secondsBeforeCrash: %d\n", secondsBeforeCrash);
        printf("       threadToCrash: %d\n", threadToCrash);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    int rc;
    eCrashSymbolTable symbol_table;

    /* Parse our arguments */
    rc = parseArguments(argc, argv);
    if (rc != 0)
    {
        return rc;
    }

    bzero(&params, sizeof(params));

    if (useSymbolTable)
    {
        BuildSymbolTable(&symbol_table);
    }

    params.filename = "eCrash.out.filename";
    params.filep = stdout;
    //params.filep = fopen("eCrash.out.filep", "w");
    params.fd = open("eCrash.out.fd", O_WRONLY | O_CREAT, S_IREAD | S_IWRITE | S_IRGRP | S_IROTH); // 0644
    if (params.fd < 0)
    {
        /* Try again, with a append */
        params.fd = open("eCrash.out.fd", O_WRONLY | O_TRUNC);
    }

    if (verbose)
    {
        params.debugLevel = ECRASH_DEBUG_VERBOSE;
    }
    params.dumpAllThreads = TRUE;
    params.useBacktraceSymbols = unsafeBacktrace;
    if (useSymbolTable)
    {
        params.symbolTable = &symbol_table;
    }
    params.signals[0] = SIGSEGV;
    params.signals[1] = SIGILL;
    params.signals[2] = SIGBUS;
    params.signals[3] = SIGABRT;

    rc = eCrash_Init(&params);
    if (rc)
    {
        printf("eCrash_Init returned %d\n", rc);
        exit(rc);
    }

    if (numThreads)
    {
        int i;
        for (i = 0 ; i < numThreads ; i++)
        {
            CreateAThread(i + 1);
        }
    }
    if (threadToCrash == 0)
    {
        int *badPtr = NULL;

        if (verbose)
        {
            printf("Sleeping for %d seconds\n", secondsBeforeCrash);
        }
        fflush(stdout);
        sleep(secondsBeforeCrash);

        printf("About to segv!\n");
        fflush(stdout);
        *badPtr = 7;
    }
    else
    {
        printf("Thread 0 Hanging forever\n");
        fflush(stdout);
        for (;;)
        {
            sleep(1);
        }
    }

    /* We never really get here */
    printf("eCrash_Uninit = %d\n", eCrash_Uninit());
    return 0;
}
