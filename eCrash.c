/*
 * File: eCrash.c
 * @author David Frascone
 * 
 *  eCrash Implementation
 *
 *  eCrash will allow you to capture stack traces in the
 *  event of a crash, and write those traces to disk, stdout,
 *  or any other file handle.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <execinfo.h>
#include <pthread.h>
#include "eCrash.h"

#define NIY()    printf("%s: Not Implemented Yet!\n", __FUNCTION__)

static eCrashParameters gbl_params;
static int gbl_fd = -1;

static int gbl_backtraceEntries;
static void **gbl_backtraceBuffer;
static char **gbl_backtraceSymbols;
static int gbl_backtraceDoneFlag = 0;

/* 
 * Private structures for our thread list
 */
typedef struct thread_list_node
{
    char *threadName;
    pthread_t thread;
    int backtraceSignal;
    sighandler_t oldHandler;
    struct thread_list_node *Next;
} ThreadListNode;

static pthread_mutex_t ThreadListMutex = PTHREAD_MUTEX_INITIALIZER;
static ThreadListNode *ThreadList = NULL;

/*********************************************************************
 *********************************************************************
 **     P  R  I  V  A  T  E      F  U  N  C  T  I  O  N  S
 *********************************************************************
 ********************************************************************/

/***
 * Insert a node into our threadList
 *
 * @param name   Text string indicating our thread
 * @param thread Our Thread Id
 * @param signo  Signal to create backtrace with
 * @param old_handler Our old handler for signo
 *
 * @returns zero on success
 */
static int addThreadToList(char *name, pthread_t thread, int signo, sighandler_t old_handler)
{
    ThreadListNode *node;

    node = malloc(sizeof(ThreadListNode));
    if (!node)
    {
        return -1;
    }

    DPRINTF(ECRASH_DEBUG_VERBOSE, "Adding thread 0x%p (%s)\n", thread, name);
    node->threadName = strdup(name);
    node->thread = thread;
    node->backtraceSignal = signo;
    node->oldHandler = old_handler;

    /* And, add it to the list */
    pthread_mutex_lock(&ThreadListMutex);
    node->Next = ThreadList;
    ThreadList = node;
    pthread_mutex_unlock(&ThreadListMutex);

    return 0;
}

/***
 * Remove a node from our threadList
 *
 * @param thread Our Thread Id
 *
 * @returns zero on success
 */
static int removeThreadFromList(pthread_t thread)
{
    ThreadListNode *Probe, *Prev = NULL;
    ThreadListNode *Removed = NULL;

    DPRINTF(ECRASH_DEBUG_VERBOSE, "Removing thread 0x%p from list . . .\n", thread);
    pthread_mutex_lock(&ThreadListMutex);
    for (Probe = ThreadList ; Probe != NULL ; Probe = Probe->Next)
    {
        if (Probe->thread == thread)
        {
            /* We found it!  Unlink it and move on! */
            Removed = Probe;
            if (Prev == NULL)
            {
                /* head of list */
                ThreadList = Probe->Next;
            }
            else
            {
                /* Prev != null, so we need to link around ourselves. */
                Prev->Next = Probe->Next;
            }
            Removed->Next = NULL;
            break;
        }

        Prev = Probe;
    }
    pthread_mutex_unlock(&ThreadListMutex);

    /* Now, if something is in Removed, free it, and return success */
    if (Removed)
    {
        DPRINTF(ECRASH_DEBUG_VERBOSE, "   Found %s -- removing\n", Removed->threadName);
        /* Reset the signal handler */
        signal(Removed->backtraceSignal, Removed->oldHandler);

        /* And free the allocated memory */
        free(Removed->threadName);
        free(Removed);

        return 0;
    }
    else
    {
        DPRINTF(ECRASH_DEBUG_VERBOSE, "   Not Found\n");
        return -1; /* Not Found */
    }
}

/***
 * Output text to a fd, looping to avoid being interrupted.
 *
 * @param str   String to output
 * @param bytes String length
 * @param fd    File descriptor to write to
 *
 * @returns bytes written, or error on failure.
 */
static int blockingWrite(char *str, int bytes, int fd)
{
    int offset = 0;
    int bytesWritten;
    int totalWritten = 0;

    while (bytes > 0)
    {
        bytesWritten = write(fd, &str[offset], bytes);
        if (bytesWritten < 1)
        {
            break;
        }
        totalWritten += bytesWritten;
        bytes -= bytesWritten;
    }

    return totalWritten;
}

/***
 * Print out a line of output to all our destinations
 *
 * One by one, output a line of text to all of our output destinations.
 *
 * Return failure if we fail to output to any of them.
 *
 * @param format   Normal printf style vararg format
 *
 * @returns bytes written, or error on failure.
 */
static int outputPrintf(char *format, ...)
{
    /* Our output line of text */
    static char outputLine[MAX_LINE_LEN];
    int bytesInLine;
    va_list ap;
    int return_value = 0;

    va_start(ap, format);

    bytesInLine = vsnprintf(outputLine, MAX_LINE_LEN - 1, format, ap);
    if (bytesInLine > -1 && bytesInLine < (MAX_LINE_LEN - 1))
    {
        /* We're a happy camper -- start printing */
        if (gbl_params.filename)
        {
            /* append to our file -- hopefully it's been opened */
            if (gbl_fd != -1)
            {
                if (blockingWrite(outputLine, bytesInLine, gbl_fd))
                {
                    return_value = -2;
                }
            }
        }

        /* Write to our file pointer */
        if (gbl_params.filep != NULL)
        {
            if (fwrite(outputLine, bytesInLine, 1, gbl_params.filep) != 1)
            {
                return_value = -3;
            }
            fflush(gbl_params.filep);
        }

        /* Write to our fd */
        if (gbl_params.fd != -1)
        {
            if (blockingWrite(outputLine, bytesInLine, gbl_params.fd))
            {
                return_value = -4;
            }
        }
    }
    else
    {
        /* We overran our string. */
        return_value = -1;
    }

    return return_value;
}

/***
 * Initialize our output (open files, etc)
 *
 * This file initializes all output streams, since we're about
 * to have output.
 *
 */
static void outputInit(void)
{
    if (gbl_params.filename)
    {
        /* First try append */
        gbl_fd = open(gbl_params.filename, O_WRONLY | O_APPEND);
        if (gbl_fd < 0)
        {
            /*                                 0644 */
            gbl_fd = open(gbl_params.filename, O_RDWR | O_CREAT, S_IREAD | S_IWRITE | S_IRGRP | S_IROTH);
            if (gbl_fd < 0)
            {
                gbl_fd = -1;
            }
        }
    }
}

/***
 * Finalize our output (close files, etc)
 *
 * This file closes all output streams.
 *
 */
static void outputFini(void)
{
    if (gbl_fd > -1)
    {
        close(gbl_fd);
    }

    if (gbl_params.filep != NULL)
    {
        fclose(gbl_params.filep);
    }

    if (gbl_params.fd > -1)
    {
        close(gbl_params.fd);
    }

    /* Just in case someone tries to call outputPrintf after outputFini */
    gbl_fd = gbl_params.fd = -1;
    gbl_params.filep = NULL;

    sync();
}

static void *lookupClosestSymbol(eCrashSymbolTable *table, void *address)
{
    int addr;
    eCrashSymbol *last = NULL;

    /* For now, use a linear lookup. */
    DPRINTF(ECRASH_DEBUG_VERBOSE, "Looking for %p in %d symbols\n", address, table->numSymbols);
    for (addr = 0 ; addr < table->numSymbols ; addr++)
    {
        DPRINTF(ECRASH_DEBUG_VERBOSE, "  Examining [%d] %p\n", addr, table->symbols[addr].address);
        if (table->symbols[addr].address > address)
        {
            break;
        }
        last = &table->symbols[addr];
    }

    /* last will either be NULL, or the last address less than the
     * one we're looking for.
     */
    DPRINTF(ECRASH_DEBUG_VERBOSE, "Returning %s (%p)\n", last ? last->function : "(nil)", last ? last->address : 0);
    return last;
}

/***
 * Dump our backtrace into a global location
 *
 * This function will dump out our backtrace into our
 * global holding area.
 *
 */
static void createGlobalBacktrace(void)
{
    gbl_backtraceEntries = backtrace(gbl_backtraceBuffer, gbl_params.maxStackDepth);

    /* This is NOT signal safe -- it calls malloc.  We need to
       let the caller pass in a pointer to a symbol table inside of
       our params. TODO */

    if (!gbl_params.symbolTable)
    {
        if (gbl_params.useBacktraceSymbols != false)
        {
            gbl_backtraceSymbols = backtrace_symbols(gbl_backtraceBuffer, gbl_backtraceEntries);
        }
    }
}

/***
 * Print out (to all the fds, etc), or global backtrace
 */
static void outputGlobalBacktrace(void)
{
    int i;

    for (i = 0 ; i < gbl_backtraceEntries ; i++)
    {
        if (gbl_params.symbolTable)
        {
            eCrashSymbol *symbol;

            symbol = lookupClosestSymbol(gbl_params.symbolTable, gbl_backtraceBuffer[i]);

            if (symbol)
            {
                outputPrintf("*      Frame %02d: %s+%u\n", i, symbol->function,
                             gbl_backtraceBuffer[i] - symbol->address);
            }
            else
            {
                outputPrintf("*      Frame %02d: %p\n", i, gbl_backtraceBuffer[i]);
            }
        }
        else
        {
            if (gbl_backtraceSymbols != false)
            {
                outputPrintf("*      Frame %02d: %s\n", i, gbl_backtraceSymbols[i]);
            }
            else
            {
                outputPrintf("*      Frame %02d: %p\n", i, gbl_backtraceBuffer[i]);
            }
        }
    }
}

/***
 * Output our current stack's backtrace
 */
static void outputBacktrace(void)
{
    createGlobalBacktrace();
    outputGlobalBacktrace();
}

static void outputBacktraceThreads(void)
{
    ThreadListNode *probe;
    int i;

    /* When we're backtracing, don't worry about the mutex . . hopefully
     * we're in a safe place.
     */

    for (probe = ThreadList ; probe ; probe = probe->Next)
    {
        gbl_backtraceDoneFlag = 0;
        pthread_kill(probe->thread, probe->backtraceSignal);
        for (i = 0 ; i < gbl_params.threadWaitTime ; i++)
        {
            if (gbl_backtraceDoneFlag)
            {
                break;
            }
            sleep(1);
        }
        if (gbl_backtraceDoneFlag)
        {
            outputPrintf("*  Backtrace of \"%s\" (0x%p)\n", probe->threadName, probe->thread);
            outputGlobalBacktrace();
        }
        else
        {
            outputPrintf("*  Error: unable to get backtrace of \"%s\" (0x%p)\n", probe->threadName,
                         probe->thread);
        }
        outputPrintf("*\n");
    }
}


/***
 * Handle signals (crash signals)
 *
 * This function will catch all crash signals, and will output the
 * crash dump.  
 *
 * It will physically write (and sync) the current thread's information
 * before it attempts to send signals to other threads.
 * 
 * @param signum Signal received.
 */
static void crash_handler(int signo)
{
    outputInit();
    outputPrintf("*********************************************************\n");
    outputPrintf("*               eCrash Crash Handler\n");
    outputPrintf("*********************************************************\n");
    outputPrintf("*\n");
    outputPrintf("*  Got a crash! signo=%d\n", signo);
    outputPrintf("*\n");
    outputPrintf("*  Offending Thread's Backtrace:\n");
    outputPrintf("*\n");
    outputBacktrace();
    outputPrintf("*\n");

    if (gbl_params.dumpAllThreads != false)
    {
        outputBacktraceThreads();
    }

    outputPrintf("*\n");
    outputPrintf("*********************************************************\n");
    outputPrintf("*               eCrash Crash Handler\n");
    outputPrintf("*********************************************************\n");

    outputFini();

    exit(signo);
}

/***
 * Handle signals (bt signals)
 *
 * This function shoudl be called to generate a crashdump into our
 * global area.  Once the dump has been completed, this function will
 * return after tickling a global.  Since mutexes are not async
 * signal safe, the main thread, after signaling us to generate our
 * own backtrace, will sleep for a few seconds waiting for us to complete.
 *
 * @param signum Signal received.
 */
static void bt_handler(int signo)
{
    createGlobalBacktrace();
    gbl_backtraceDoneFlag = 1;
}

/***
 * Validate a passed-in symbol table
 *
 * For now, just print it out (if verbose), and make sure it's
 * sorted and none of the pointers are zero.
 */
static int ValidateSymbolTable(void)
{
    int i;
    int rc = 0;
    unsigned long lastAddress = 0;

    /* Get out of here if the table is empty */
    if (!gbl_params.symbolTable)
    {
        return 0;
    }

    /* Dump it in verbose mode */
    DPRINTF(ECRASH_DEBUG_VERBOSE, "Symbol Table Provided with %d symbols\n", gbl_params.symbolTable->numSymbols);
    for (i = 0 ; i < gbl_params.symbolTable->numSymbols ; i++)
    {
        /* Dump it in verbose mode */
        DPRINTF(ECRASH_DEBUG_VERBOSE, "%-30s %p\n", gbl_params.symbolTable->symbols[i].function,
                gbl_params.symbolTable->symbols[i].address);
        if (lastAddress > (unsigned long)gbl_params.symbolTable->symbols[i].address)
        {
            DPRINTF(ECRASH_DEBUG_ERROR, "Error: symbol table is not sorted (last=%p, current=%p)\n",
                    (void *)lastAddress, gbl_params.symbolTable->symbols[i].address);
            rc = -1;
        }

    }

    return rc;

}

/*********************************************************************
 *********************************************************************
 **      P  U  B  L  I  C      F  U  N  C  T  I  O  N  S
 *********************************************************************
 ********************************************************************/

/***
 * Initialize eCrash.
 * 
 * This function must be called before calling any other eCrash
 * functions.  It sets up the global behavior of the system, and
 * registers the calling thread for crash dumps.
 *
 * @param params Our input parameters.  The passed in structure will be copied.
 *
 * @return Zero on success.
 */
int eCrash_Init(eCrashParameters *params)
{
    int sigIndex;
    int ret = 0;
#ifdef DO_SIGNALS_RIGHT
    sigset_t blocked;
    struct sigaction act;
#endif

    DPRINTF(ECRASH_DEBUG_VERY_VERBOSE, "Init Starting params = %p\n", params);

    /* Allocate our backtrace area */
    gbl_backtraceBuffer = malloc(sizeof(void *) * (params->maxStackDepth + 5));

#ifdef DO_SIGNALS_RIGHT
    sigemptyset(&blocked);
    act.sa_sigaction = crash_handler;
    act.sa_mask = blocked;
    act.sa_flags = SA_SIGINFO;
#endif

    if (params != NULL)
    {
        /* Make ourselves a global copy of params. */
        gbl_params = *params;
        gbl_params.filename = strdup(params->filename);

        /* Set our defaults, if they weren't specified */
        if (gbl_params.maxStackDepth == 0)
        {
            gbl_params.maxStackDepth = ECRASH_DEFAULT_STACK_DEPTH;
        }

        if (gbl_params.defaultBacktraceSignal == 0)
        {
            gbl_params.defaultBacktraceSignal = ECRASH_DEFAULT_BACKTRACE_SIGNAL;
        }

        if (gbl_params.threadWaitTime == 0)
        {
            gbl_params.threadWaitTime = ECRASH_DEFAULT_THREAD_WAIT_TIME;
        }

        if (gbl_params.debugLevel == 0)
        {
            gbl_params.debugLevel = ECRASH_DEBUG_DEFAULT;
        }

        /* Copy our symbol table */
        if (gbl_params.symbolTable)
        {
            DPRINTF(ECRASH_DEBUG_VERBOSE, "symbolTable @ %p -- %d symbols\n", gbl_params.symbolTable,
                    gbl_params.symbolTable->numSymbols);
            /* Make a copy of our symbol table */
            gbl_params.symbolTable = malloc(sizeof(eCrashSymbolTable));
            memcpy(gbl_params.symbolTable, params->symbolTable, sizeof(eCrashSymbolTable));

            /* Now allocate / copy the actual table. */
            gbl_params.symbolTable->symbols = malloc(sizeof(eCrashSymbol) * gbl_params.symbolTable->numSymbols);
            memcpy(gbl_params.symbolTable->symbols, params->symbolTable->symbols,
                   sizeof(eCrashSymbol) * gbl_params.symbolTable->numSymbols);

            ValidateSymbolTable();
        }

        /* And, finally, register for our signals */
        for (sigIndex = 0 ; gbl_params.signals[sigIndex] != 0 ; sigIndex++)
        {
            DPRINTF(ECRASH_DEBUG_VERY_VERBOSE, "   Catching signal[%d] %d\n", sigIndex, gbl_params.signals[sigIndex]);

            /* I know there's a better way to catch signals with pthreads.
             * I'll do it later TODO
             */
            signal(gbl_params.signals[sigIndex], crash_handler);
        }
    }
    else
    {
        DPRINTF(ECRASH_DEBUG_ERROR, "   Error:  Null Params!\n");
        ret = -1;
    }
    DPRINTF(ECRASH_DEBUG_VERY_VERBOSE, "Init Complete ret=%d\n", ret);

    return ret;
}

/***
 * UnInitialize eCrash.
 * 
 * This function may be called to de-activate eCrash, release the
 * signal handlers, and free any memory allocated by eCrash.
 *
 * @return Zero on success.
 */
int eCrash_Uninit(void)
{
    NIY();

    return 0;
}

/***
 * Register a thread for backtracing on crash.
 * 
 * This function must be called by any thread wanting it's stack
 * dumped in the event of a crash.  The thread my specify what 
 * signal should be used, or the default, SIGUSR1 will be used.
 *
 * @param signo Signal to use to generate dump (default: SIGUSR1)
 *
 * @return Zero on success.
 */
int eCrash_RegisterThread(char *name, int signo)
{
    sighandler_t old_handler;

    /* Register for our signal */
    if (signo == 0)
    {
        signo = gbl_params.defaultBacktraceSignal;
    }

    old_handler = signal(signo, bt_handler);
    return addThreadToList(name, pthread_self(), signo, old_handler);

}

/***
 * Un-register a thread for stack dumps.
 * 
 * This function may be called to un-register any previously 
 * registered thread.
 *
 * @return Zero on success.
 */
int eCrash_UnregisterThread(void)
{
    return removeThreadFromList(pthread_self());
}

