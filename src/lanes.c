/*
 * LANES.C   	                          Copyright (c) 2007-08, Asko Kauppi
 *
 * Multithreading in Lua.
 * 
 * History:
 *      24-Jun-08 .. 14-Aug-08 AKa: Major revise, Lanes 2008 version (2.0 rc1)
 *          ...
 *      18-Sep-06 AKa: Started the module.
 *
 * Platforms (tested internally):
 *      OS X (10.5.4 PowerPC/Intel)
 *      Linux x86 (Ubuntu 8.04)
 *      Win32 (Windows XP Home SP2, Visual C++ 2005/2008 Express)
 *      PocketPC (TBD)
 *
 * Platforms (tested externally):
 *      Win32 (MSYS) by Ross Berteig.
 *
 * Platforms (testers appreciated):
 *      Win64 - should work???
 *      Linux x64 - should work
 *      FreeBSD - should work
 *      QNX - porting shouldn't be hard
 *      Sun Solaris - porting shouldn't be hard
 *
 * References:
 *      "Porting multithreaded applications from Win32 to Mac OS X":
 *      <http://developer.apple.com/macosx/multithreadedprogramming.html>
 *
 *      Pthreads:
 *      <http://vergil.chemistry.gatech.edu/resources/programming/threads.html>
 *
 *      MSDN: <http://msdn2.microsoft.com/en-us/library/ms686679.aspx>
 *
 *      <http://ridiculousfish.com/blog/archives/2007/02/17/barrier>
 *
 * Defines:
 *      -DLINUX_SCHED_RR: all threads are lifted to SCHED_RR category, to
 *          allow negative priorities (-2,-1) be used. Even without this,
 *          using priorities will require 'sudo' privileges on Linux.
 *
 *		-DUSE_PTHREAD_TIMEDJOIN: use 'pthread_timedjoin_np()' for waiting
 *          for threads with a timeout. This changes the thread cleanup
 *          mechanism slightly (cleans up at the join, not once the thread
 *          has finished). May or may not be a good idea to use it.
 *          Available only in selected operating systems (Linux).
 *
 * Bugs:
 *
 * To-do:
 *
 *      ...
 */

const char *VERSION= "2.0";

/*
===============================================================================

Copyright (C) 2007-08 Asko Kauppi <akauppi@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

===============================================================================
*/
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"

#include "threading.h"
#include "tools.h"

#if !((defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC))
# include <sys/time.h>
#endif

/* geteuid() */
#ifdef PLATFORM_LINUX
# include <unistd.h>
# include <sys/types.h>
#endif

/* The selected number is not optimal; needs to be tested. Even using just
* one keeper state may be good enough (depends on the number of Lindas used
* in the applications).
*/
#define KEEPER_STATES_N 1   // 6

/*
* Lua code for the keeper states (baked in)
*/
char keeper_chunk[]= 
#include "keeper.lch"

struct s_lane;
static bool_t cancel_test( lua_State *L );
static void cancel_error( lua_State *L );

#define CANCEL_TEST_KEY ((void*)cancel_test)    // used as registry key
#define CANCEL_ERROR ((void*)cancel_error)      // 'cancel_error' sentinel

struct s_Linda;

#if 1
# define DEBUG_SIGNAL( msg, signal_ref ) /* */
#else
# define DEBUG_SIGNAL( msg, signal_ref ) \
    { int i; unsigned char *ptr; char buf[999]; \
      sprintf( buf, ">>> " msg ": %p\t", (signal_ref) ); \
      ptr= (unsigned char *)signal_ref; \
      for( i=0; i<sizeof(*signal_ref); i++ ) { \
        sprintf( strchr(buf,'\0'), "%02x %c ", ptr[i], ptr[i] ); \
      } \
      fprintf( stderr, "%s\n", buf ); \
    }
#endif


/*---=== Serialize require ===---
*/

static MUTEX_T require_cs;

//---
// [val]= new_require( ... )
//
// Call 'old_require' but only one lane at a time.
//
// Upvalues: [1]: original 'require' function
//
static int new_require( lua_State *L ) {
    int rc;
    int args= lua_gettop(L);

  STACK_GROW(L,1);
  STACK_CHECK(L)
    
    // Using 'lua_pcall()' to catch errors; otherwise a failing 'require' would
    // leave us locked, blocking any future 'require' calls from other lanes.
    //
    MUTEX_LOCK( &require_cs );
    {
        lua_pushvalue( L, lua_upvalueindex(1) );
        lua_insert( L, 1 );

        rc= lua_pcall( L, args, 1 /*retvals*/, 0 /*errfunc*/ );
            //
            // LUA_ERRRUN / LUA_ERRMEM
    }
    MUTEX_UNLOCK( &require_cs );

    if (rc) lua_error(L);   // error message already at [-1]

  STACK_END(L,0)
    return 1;
}

/*
* Serialize calls to 'require', if it exists
*/
static 
void serialize_require( lua_State *L ) {

  STACK_GROW(L,1);  
  STACK_CHECK(L)
    
    // Check 'require' is there; if not, do nothing
    //
    lua_getglobal( L, "require" );
    if (lua_isfunction( L, -1 )) {
        // [-1]: original 'require' function

        lua_pushcclosure( L, new_require, 1 /*upvalues*/ );
        lua_setglobal( L, "require" );

    } else {
        // [-1]: nil
        lua_pop(L,1);
    }

  STACK_END(L,0)
}


/*---=== Keeper states ===---
*/

/*
* Pool of keeper states
*
* Access to keeper states is locked (only one OS thread at a time) so the 
* bigger the pool, the less chances of unnecessary waits. Lindas map to the
* keepers randomly, by a hash.
*/
struct s_Keeper {
    MUTEX_T lock_;
    lua_State *L;
} keeper[ KEEPER_STATES_N ];

/* We could use an empty table in 'keeper.lua' as the sentinel, but maybe
* checking for a lightuserdata is faster.
*/
static bool_t nil_sentinel;

/*
* Initialize keeper states
*
* If there is a problem, return an error message (NULL for okay).
*
* Note: Any problems would be design flaws; the created Lua state is left
*       unclosed, because it does not really matter. In production code, this
*       function never fails.
*/
static const char *init_keepers(void) {
    unsigned int i;
    for( i=0; i<KEEPER_STATES_N; i++ ) {
        
        // Initialize Keeper states with bare minimum of libs (those required
        // by 'keeper.lua')
        //
        lua_State *L= luaL_newstate();
        if (!L) return "out of memory";

        luaG_openlibs( L, "io,table" );     // 'io' for debugging messages

        lua_pushlightuserdata( L, &nil_sentinel );
        lua_setglobal( L, "nil_sentinel" );

        // Read in the preloaded chunk (and run it)
        //
        if (luaL_loadbuffer( L, keeper_chunk, sizeof(keeper_chunk), "=lanes_keeper" ))
            return "luaL_loadbuffer() failed";   // LUA_ERRMEM

        if (lua_pcall( L, 0 /*args*/, 0 /*results*/, 0 /*errfunc*/ )) {
            // LUA_ERRRUN / LUA_ERRMEM / LUA_ERRERR
            //
            const char *err= lua_tostring(L,-1);
            assert(err);
            return err;
        }

        MUTEX_INIT( &keeper[i].lock_ );
        keeper[i].L= L;
    }
    return NULL;    // ok
}

static 
struct s_Keeper *keeper_acquire( const void *ptr ) {
    /*
    * Any hashing will do that maps pointers to 0..KEEPER_STATES_N-1 
    * consistently.
    *
    * Pointers are often aligned by 8 or so - ignore the low order bits
    */
    unsigned int i= ((unsigned long)(ptr) >> 3) % KEEPER_STATES_N;
    struct s_Keeper *K= &keeper[i];

    MUTEX_LOCK( &K->lock_ );
    return K;
}

static 
void keeper_release( struct s_Keeper *K ) {
    MUTEX_UNLOCK( &K->lock_ );
}

/*
* Call a function ('func_name') in the keeper state, and pass on the returned
* values to 'L'.
*
* 'linda':          deep Linda pointer (used only as a unique table key, first parameter)
* 'starting_index': first of the rest of parameters (none if 0)
*
* Returns:  number of return values (pushed to 'L')
*/
static
int keeper_call( lua_State* K, const char *func_name, 
                  lua_State *L, struct s_Linda *linda, uint_t starting_index ) {

    int args= starting_index ? (lua_gettop(L) - starting_index +1) : 0;
    int Ktos= lua_gettop(K);
    int retvals;

    lua_getglobal( K, func_name );
    ASSERT_L( lua_isfunction(K,-1) );

    STACK_GROW( K, 1 );
    lua_pushlightuserdata( K, linda );

    luaG_inter_copy( L,K, args );   // L->K
    lua_call( K, 1+args, LUA_MULTRET );

    retvals= lua_gettop(K) - Ktos;

    luaG_inter_move( K,L, retvals );    // K->L
    return retvals;
}


/*---=== Linda ===---
*/

/*
* Actual data is kept within a keeper state, which is hashed by the 's_Linda'
* pointer (which is same to all userdatas pointing to it).
*/
struct s_Linda {
    SIGNAL_T read_happened;
    SIGNAL_T write_happened;
};

static int LG_linda_id( lua_State* );

#define lua_toLinda(L,n) ((struct s_Linda *)luaG_todeep( L, LG_linda_id, n ))


/*
* bool= linda_send( linda_ud, [timeout_secs=-1,] key_num|str|bool|lightuserdata, ... )
*
* Send one or more values to a Linda. If there is a limit, all values must fit.
*
* Returns:  'true' if the value was queued
*           'false' for timeout (only happens when the queue size is limited)
*/
LUAG_FUNC( linda_send ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );
    bool_t ret;
    bool_t cancel= FALSE;
    struct s_Keeper *K;
    time_d timeout= -1.0;
    uint_t key_i= 2;    // index of first key, if timeout not there

    if (lua_isnumber(L,2)) {
        timeout= SIGNAL_TIMEOUT_PREPARE( lua_tonumber(L,2) );
        key_i++;
    } else if (lua_isnil(L,2))
        key_i++;

    if (lua_isnil(L,key_i))
        luaL_error( L, "nil key" );

    STACK_GROW(L,1);

    K= keeper_acquire( linda );
    {
        lua_State *KL= K->L;    // need to do this for 'STACK_CHECK'
STACK_CHECK(KL)
        while(TRUE) {
            int pushed;
        
STACK_MID(KL,0)
            pushed= keeper_call( K->L, "send", L, linda, key_i );
            ASSERT_L( pushed==1 );
        
            ret= lua_toboolean(L,-1);
            lua_pop(L,1);
        
            if (ret) {
                // Wake up ALL waiting threads
                //
                SIGNAL_ALL( &linda->write_happened );
                break;

            } else if (timeout==0.0) {
                break;  /* no wait; instant timeout */

            } else {
                /* limit faced; push until timeout */
                    
                cancel= cancel_test( L );   // testing here causes no delays
                if (cancel) break;

                // K lock will be released for the duration of wait and re-acquired
                //
                if (!SIGNAL_WAIT( &linda->read_happened, &K->lock_, timeout ))
                    break;  // timeout
            }
        }
STACK_END(KL,0)
    }
    keeper_release(K);

    if (cancel)
        cancel_error(L);
    
    lua_pushboolean( L, ret );
    return 1;
}


/*
* [val, key]= linda_receive( linda_ud, [timeout_secs_num=-1], key_num|str|bool|lightuserdata [, ...] )
*
* Receive a value from Linda, consuming it.
*
* Returns:  value received (which is consumed from the slot)
*           key which had it
*/
LUAG_FUNC( linda_receive ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );
    int pushed;
    bool_t cancel= FALSE;
    struct s_Keeper *K;
    time_d timeout= -1.0;
    uint_t key_i= 2;

    if (lua_isnumber(L,2)) {
        timeout= SIGNAL_TIMEOUT_PREPARE( lua_tonumber(L,2) );
        key_i++;
    } else if (lua_isnil(L,2))
        key_i++;

    K= keeper_acquire( linda );
    {
        while(TRUE) {
            pushed= keeper_call( K->L, "receive", L, linda, key_i );
            if (pushed) {
                ASSERT_L( pushed==2 );

                // To be done from within the 'K' locking area
                //
                SIGNAL_ALL( &linda->read_happened );
                break;

            } else if (timeout==0.0) {
                break;  /* instant timeout */

            } else {    /* nothing received; wait until timeout */
    
                cancel= cancel_test( L );   // testing here causes no delays
                if (cancel) break;

                // Release the K lock for the duration of wait, and re-acquire
                //
                if (!SIGNAL_WAIT( &linda->write_happened, &K->lock_, timeout ))
                    break;
            }
        }
    }
    keeper_release(K);

    if (cancel)
        cancel_error(L);

    return pushed;
}


/*
* = linda_set( linda_ud, key_num|str|bool|lightuserdata [,value] )
*
* Set a value to Linda.
*
* Existing slot value is replaced, and possible queue entries removed.
*/
LUAG_FUNC( linda_set ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );
    bool_t has_value= !lua_isnil(L,3);

    struct s_Keeper *K= keeper_acquire( linda );
    {
        int pushed= keeper_call( K->L, "set", L, linda, 2 );
        ASSERT_L( pushed==0 );

        /* Set the signal from within 'K' locking.
        */
        if (has_value) {
            SIGNAL_ALL( &linda->write_happened );
        }
    }
    keeper_release(K);

    return 0;
}


/*
* [val]= linda_get( linda_ud, key_num|str|bool|lightuserdata )
*
* Get a value from Linda.
*/
LUAG_FUNC( linda_get ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );
    int pushed;

    struct s_Keeper *K= keeper_acquire( linda );
    {
        pushed= keeper_call( K->L, "get", L, linda, 2 );
        ASSERT_L( pushed==0 || pushed==1 );
    }
    keeper_release(K);

    return pushed;
}


/*
* = linda_limit( linda_ud, key_num|str|bool|lightuserdata, uint [, ...] )
*
* Set limits to 1 or more Linda keys.
*/
LUAG_FUNC( linda_limit ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );

    struct s_Keeper *K= keeper_acquire( linda );
    {
        int pushed= keeper_call( K->L, "limit", L, linda, 2 );
        ASSERT_L( pushed==0 );
    }
    keeper_release(K);

    return 0;
}


/*
* lightuserdata= linda_deep( linda_ud )
*
* Return the 'deep' userdata pointer, identifying the Linda.
*
* This is needed for using Lindas as key indices (timer system needs it);
* separately created proxies of the same underlying deep object will have
* different userdata and won't be known to be essentially the same deep one
* without this.
*/
LUAG_FUNC( linda_deep ) {
    struct s_Linda *linda= lua_toLinda( L, 1 );
    lua_pushlightuserdata( L, linda );      // just the address
    return 1;
}


/*
* Identity function of a shared userdata object.
* 
*   lightuserdata= linda_id( "new" [, ...] )
*   = linda_id( "delete", lightuserdata )
*
* Creation and cleanup of actual 'deep' objects. 'luaG_...' will wrap them into
* regular userdata proxies, per each state using the deep data.
*
*   tbl= linda_id( "metatable" )
*
* Returns a metatable for the proxy objects ('__gc' method not needed; will
* be added by 'luaG_...')
*
*   = linda_id( str, ... )
*
* For any other strings, the ID function must not react at all. This allows
* future extensions of the system. 
*/
LUAG_FUNC( linda_id ) {
    const char *which= lua_tostring(L,1);

    if (strcmp( which, "new" )==0) {
        struct s_Linda *s;

        /* We don't use any parameters, but one could (they're at [2..TOS])
        */
        ASSERT_L( lua_gettop(L)==1 );

        /* The deep data is allocated separately of Lua stack; we might no
        * longer be around when last reference to it is being released.
        * One can use any memory allocation scheme.
        */
        s= (struct s_Linda *) malloc( sizeof(struct s_Linda) );
        ASSERT_L(s);

        SIGNAL_INIT( &s->read_happened );
        SIGNAL_INIT( &s->write_happened );

        lua_pushlightuserdata( L, s );
        return 1;

    } else if (strcmp( which, "delete" )==0) {
        struct s_Keeper *K;
        struct s_Linda *s= lua_touserdata(L,2);
        ASSERT_L(s);

        /* Clean associated structures in the keeper state.
        */
        K= keeper_acquire(s);
        {
            keeper_call( K->L, "clear", L, s, 0 );
        }
        keeper_release(K);

        /* There aren't any lanes waiting on these lindas, since all proxies
        * have been gc'ed. Right?
        */
        SIGNAL_FREE( &s->read_happened );
        SIGNAL_FREE( &s->write_happened );
        free(s);

        return 0;

    } else if (strcmp( which, "metatable" )==0) {

      STACK_CHECK(L)
        lua_newtable(L);
        lua_newtable(L);
            //
            // [-2]: linda metatable
            // [-1]: metatable's to-be .__index table
    
        lua_pushcfunction( L, LG_linda_send );
        lua_setfield( L, -2, "send" );
    
        lua_pushcfunction( L, LG_linda_receive );
        lua_setfield( L, -2, "receive" );
    
        lua_pushcfunction( L, LG_linda_limit );
        lua_setfield( L, -2, "limit" );

        lua_pushcfunction( L, LG_linda_set );
        lua_setfield( L, -2, "set" );
    
        lua_pushcfunction( L, LG_linda_get );
        lua_setfield( L, -2, "get" );

        lua_pushcfunction( L, LG_linda_deep );
        lua_setfield( L, -2, "deep" );

        lua_setfield( L, -2, "__index" );
      STACK_END(L,1)
    
        return 1;
    }
    
    return 0;   // unknown request, be quiet
}


/*---=== Threads ===---
*/

// NOTE: values to be changed by either thread, during execution, without
//       locking, are marked "volatile"
//
struct s_lane {
    THREAD_T thread;
        //
        // M: sub-thread OS thread
        // S: not used

    lua_State *L;
        //
        // M: prepares the state, and reads results
        // S: while S is running, M must keep out of modifying the state

    volatile enum e_status status;
        // 
        // M: sets to PENDING (before launching)
        // S: updates -> RUNNING/WAITING -> DONE/ERROR_ST/CANCELLED
    
    volatile bool_t cancel_request;
        //
        // M: sets to FALSE, flags TRUE for cancel request
        // S: reads to see if cancel is requested

#if !( (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN) )
    SIGNAL_T done_signal_;
        //
        // M: Waited upon at lane ending  (if Posix with no PTHREAD_TIMEDJOIN)
        // S: sets the signal once cancellation is noticed (avoids a kill)

    MUTEX_T done_lock_;
        // 
        // Lock required by 'done_signal' condition variable, protecting
        // lane status changes to DONE/ERROR_ST/CANCELLED.
#endif

    volatile enum { 
        NORMAL,         // normal master side state
        SELFDESTRUCT,   // free-running thread, handle has been gc'ed. Shall clean after itself
        KILLED          // issued an OS kill
    } mstatus;
        //
        // M: possibly sets to SELFDESTRUCT or KILLED
        // S: read at exit; if SELFDESTRUCT clears up the data structure
};

// To allow free-running threads (longer lifespan than the handle's)
// 'struct s_lane' are malloc/free'd and the handle only carries a pointer.
// This is not deep userdata since the handle's not portable among lanes.
//
#define lua_toLane(L,i)  (* ((struct s_lane**) lua_touserdata(L,i)))


/*
* Check if the thread in question ('L') has been signalled for cancel.
*
* Called by cancellation hooks and/or pending Linda operations (because then
* the check won't affect performance).
*
* Returns TRUE if any locks are to be exited, and 'cancel_error()' called,
* to make execution of the lane end.
*/
static bool_t cancel_test( lua_State *L ) {
    struct s_lane *s;

    STACK_GROW(L,1);

  STACK_CHECK(L)
    lua_pushlightuserdata( L, CANCEL_TEST_KEY );
    lua_rawget( L, LUA_REGISTRYINDEX );
    s= lua_touserdata( L, -1 );     // lightuserdata (true 's_lane' pointer) / nil
    lua_pop(L,1);
  STACK_END(L,0)

    // 's' is NULL for the original main state (no-one can cancel that)
    //
    return s && s->cancel_request;
}

static void cancel_error( lua_State *L ) {
    STACK_GROW(L,1);
    lua_pushlightuserdata( L, CANCEL_ERROR );    // special error value
    lua_error(L);   // no return
}

static void cancel_hook( lua_State *L, lua_Debug *ar ) {
    (void)ar;
    if (cancel_test(L)) cancel_error(L);
}


//---
// = _single( [cores_uint=1] )
//
// Limits the process to use only 'cores' CPU cores. To be used for performance
// testing on multicore devices. DEBUGGING ONLY!
//
LUAG_FUNC( _single ) {
	uint_t cores= luaG_optunsigned(L,1,1);

#ifdef PLATFORM_OSX
  #ifdef _UTILBINDTHREADTOCPU
	if (cores > 1) luaL_error( L, "Limiting to N>1 cores not possible." );
    // requires 'chudInitialize()'
    utilBindThreadToCPU(0);     // # of CPU to run on (we cannot limit to 2..N CPUs?)
  #else
    luaL_error( L, "Not available: compile with _UTILBINDTHREADTOCPU" );
  #endif
#else
    luaL_error( L, "not implemented!" );
#endif
	(void)cores;
	
	return 0;
}


/*
* str= lane_error( error_val|str )
*
* Called if there's an error in some lane; add call stack to error message 
* just like 'lua.c' normally does.
*
* ".. will be called with the error message and its return value will be the 
*     message returned on the stack by lua_pcall."
*/
static int lane_error( lua_State *L ) {
    lua_Debug ar;

    assert( lua_gettop(L)==1 );

    // [1]: plain error message

    if (lua_type(L,1) == LUA_TSTRING) {
        if (lua_getstack(L, 1 /*level*/, &ar)) {
            lua_getinfo(L, "Sl", &ar);
            if (ar.currentline > 0) {
                STACK_GROW(L,1);
                lua_pushfstring( L, "%s:%d: ", ar.short_src, ar.currentline );
                lua_concat( L, 2 );
            }
        }
    }
    return 1;
}


//---
#if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC)
  static THREAD_RETURN_T __stdcall lane_main( void *vs )
#else
  static THREAD_RETURN_T lane_main( void *vs )
#endif
{
    struct s_lane *s= (struct s_lane *)vs;
    int rc;

    s->status= RUNNING;  // PENDING -> RUNNING

    STACK_GROW( s->L, 1 );
    lua_pushcfunction( s->L, lane_error );
    lua_insert( s->L, 1 );

    // [1]: error handler
    // [2]: function to run
    // [3..top]: parameters
    //
    rc= lua_pcall( s->L, lua_gettop(s->L)-2, LUA_MULTRET, 0 /*error handler (no if 0, yes if 1)*/ );
        //
        // 0: no error
        // LUA_ERRRUN: a runtime error (error pushed on stack)
        // LUA_ERRMEM: memory allocation error
        // LUA_ERRERR: error while running the error handler (if any)

    lua_remove(s->L,1);    // remove error handler

    if (s->mstatus == SELFDESTRUCT) {
        // We're a free-running thread and no-one's there to clean us up.
        //
        lua_close( s->L );

    #if !( (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN) )
        SIGNAL_FREE( &s->done_signal_ );
        MUTEX_FREE( &s->done_lock_ );
    #endif
        free(s);

    } else {
        // leave results (1..top) or error message (top) on the stack - master will copy them

        enum e_status st= 
            (rc==0) ? DONE 
                    : (lua_touserdata(s->L,-1)==CANCEL_ERROR) ? CANCELLED 
                    : ERROR_ST;

        // Posix no PTHREAD_TIMEDJOIN:
        // 		'done_lock' protects the -> DONE|ERROR_ST|CANCELLED state change
        //
    #if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN)
        s->status= st;
    #else
        MUTEX_LOCK( &s->done_lock_ );
        {
            s->status= st;
            SIGNAL_ONE( &s->done_signal_ );   // wake up master (while 's->done_lock' is on)
        }
        MUTEX_UNLOCK( &s->done_lock_ );
    #endif
    }

    return 0;   // ignored
}


//---
// lane_ud= thread_new( function, [libs_str], 
//                          [cancelstep_uint=0], 
//                          [prio_int=0],
//                          [globals_tbl],
//                          [... args ...] )
//
// Upvalues: metatable to use for 'lane_ud'
//
LUAG_FUNC( thread_new )
{
    lua_State *L2;
    struct s_lane *s;
    struct s_lane **ud;

    const char *libs= lua_tostring( L, 2 );
    uint_t cs= luaG_optunsigned( L, 3,0);
    int prio= luaL_optinteger( L, 4,0);
    uint_t glob= luaG_isany(L,5) ? 5:0;

    #define FIXED_ARGS (5)
    uint_t args= lua_gettop(L) - FIXED_ARGS;

    if (prio < THREAD_PRIO_MIN || prio > THREAD_PRIO_MAX) {
        luaL_error( L, "Priority out of range: %d..+%d (%d)", 
                            THREAD_PRIO_MIN, THREAD_PRIO_MAX, prio );
    }

    /* --- Create and prepare the sub state --- */

    L2 = luaL_newstate();   // uses standard 'realloc()'-based allocator,
                            // sets the panic callback

    if (!L2) luaL_error( L, "'luaL_newstate()' failed; out of memory" );

    STACK_GROW( L,2 );

    // Setting the globals table (needs to be done before loading stdlibs,
    // and the lane function)
    //
    if (glob!=0) {
STACK_CHECK(L)
        if (!lua_istable(L,glob)) 
            luaL_error( L, "Expected table, got %s", luaG_typename(L,glob) );

        lua_pushvalue( L, glob );
        luaG_inter_move( L,L2, 1 );     // moves the table to L2

        // L2 [-1]: table of globals

        // "You can change the global environment of a Lua thread using lua_replace"
        // (refman-5.0.pdf p. 30) 
        //
        lua_replace( L2, LUA_GLOBALSINDEX );
STACK_END(L,0)
    }

    // Selected libraries
    //
    if (libs) {
        const char *err= luaG_openlibs( L2, libs );
        ASSERT_L( !err );   // bad libs should have been noticed by 'lanes.lua'

        serialize_require( L2 );
    }

    // Lane main function
    //
STACK_CHECK(L)
    lua_pushvalue( L, 1 );
    luaG_inter_move( L,L2, 1 );    // L->L2
STACK_MID(L,0)

    ASSERT_L( lua_gettop(L2) == 1 );
    ASSERT_L( lua_isfunction(L2,1) );

    // revive arguments
    //
    if (args) luaG_inter_copy( L,L2, args );    // L->L2
STACK_MID(L,0)

ASSERT_L( (uint_t)lua_gettop(L2) == 1+args );
ASSERT_L( lua_isfunction(L2,1) );

    // 's' is allocated from heap, not Lua, since its life span may surpass 
    // the handle's (if free running thread)
    //
    ud= lua_newuserdata( L, sizeof(struct s_lane*) );
    ASSERT_L(ud);

    s= *ud= malloc( sizeof(struct s_lane) );
    ASSERT_L(s);

    //memset( s, 0, sizeof(struct s_lane) );
    s->L= L2;
    s->status= PENDING;
    s->cancel_request= FALSE;

#if !( (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN) )
    MUTEX_INIT( &s->done_lock_ );
    SIGNAL_INIT( &s->done_signal_ );
#endif
    s->mstatus= NORMAL;

    // Set metatable for the userdata
    //
    lua_pushvalue( L, lua_upvalueindex(1) );
    lua_setmetatable( L, -2 );
STACK_MID(L,1)

    // Place 's' to registry, for 'cancel_test()' (even if 'cs'==0 we still
    // do cancel tests at pending send/receive).
    //
    lua_pushlightuserdata( L2, CANCEL_TEST_KEY );
    lua_pushlightuserdata( L2, s );
    lua_rawset( L2, LUA_REGISTRYINDEX );

    if (cs) {
        lua_sethook( L2, cancel_hook, LUA_MASKCOUNT, cs );
    }

    THREAD_CREATE( &s->thread, lane_main, s, prio );
STACK_END(L,1)

    return 1;
}


//---
// = thread_gc( lane_ud )
//
// Cleanup for a thread userdata. If the thread is still executing, leave it
// alive as a free-running thread (will clean up itself).
//
// Why not cancel/kill a loose thread: 
// At least timer system uses a free-running thread, they should be handy
// and the issue of cancelling/killing threads at gc is not very nice, either
// (would easily cause waits at gc cycle, which we don't want).
//
LUAG_FUNC( thread_gc ) {
    struct s_lane *s= lua_toLane(L,1);

    // We can read 's->status' without locks, but not wait for it
    //
    if (s->status < DONE) {
        // 
        s->mstatus= SELFDESTRUCT;
        return 0;

    } else if (s->mstatus==KILLED) {
        // Make sure a kill has proceeded, before cleaning up the data structure.
        //
        // If not doing 'THREAD_WAIT()' we should close the Lua state here
        // (can it be out of order, since we killed the lane abruptly?)
        //
#if 0
        lua_close( s->L );
#else
fprintf( stderr, "** Joining with a killed thread (needs testing) **" );
#if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN)
        THREAD_WAIT( &s->thread, -1 );
#else
        THREAD_WAIT( &s->thread, &s->done_signal_, &s->done_lock_, &s->status, -1 );
#endif
fprintf( stderr, "** Joined ok **" );
#endif
    }

    // Clean up after a (finished) thread
    //
#if (! ((defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN)))
    SIGNAL_FREE( &s->done_signal_ );
    MUTEX_FREE( &s->done_lock_ );
    free(s);
#endif

    return 0;
}


//---
// = thread_cancel( lane_ud [,timeout_secs=0.0] [,force_kill_bool=false] )
//
// The originator thread asking us specifically to cancel the other thread.
//
// 'timeout': <0: wait forever, until the lane is finished
//            0.0: just signal it to cancel, no time waited
//            >0: time to wait for the lane to detect cancellation
//
// 'force_kill': if true, and lane does not detect cancellation within timeout,
//            it is forcefully killed. Using this with 0.0 timeout means just kill
//            (unless the lane is already finished).
//
// Returns: true if the lane was already finished (DONE/ERROR_ST/CANCELLED) or if we
//          managed to cancel it.
//          false if the cancellation timed out, or a kill was needed.
//
LUAG_FUNC( thread_cancel )
{
    struct s_lane *s= lua_toLane(L,1);
    double secs= 0.0;
    uint_t force_i=2;
    bool_t kill, done=TRUE;
    
    if (lua_isnumber(L,2)) {
        secs= lua_tonumber(L,2);
        force_i++;
    } else if (lua_isnil(L,2))
        force_i++;

    kill= lua_toboolean(L,force_i);     // FALSE if nothing there
    
    // We can read 's->status' without locks, but not wait for it (if Posix no PTHREAD_TIMEDJOIN)
    //
    if (s->status < DONE) {
        
        s->cancel_request= TRUE;    // it's now signalled to stop

        done= 
#if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN)
            THREAD_WAIT( &s->thread, secs );
#else
            THREAD_WAIT( &s->thread, &s->done_signal_, &s->done_lock_, &s->status, secs );
#endif

        if ((!done) && kill) {
            // Killing is asynchronous; we _will_ wait for it to be done at
            // GC, to make sure the data structure can be released (alternative
            // would be use of "cancellation cleanup handlers" that at least
            // PThread seems to have). TBD?
            //
            THREAD_KILL( &s->thread );
            s->mstatus= KILLED;     // may be useful info for GC
        }
    }

    lua_pushboolean( L, done );
    return 1;
}


//---
// str= thread_status( lane_ud )
//
// Returns: "pending"   not started yet
//          -> "running"   started, doing its work..
//             <-> "waiting"   blocked in a receive()
//                -> "done"     finished, results are there
//                   / "error"     finished at an error, error value is there
//                   / "cancelled"   execution cancelled by M (state gone)
//
LUAG_FUNC( thread_status )
{
    struct s_lane *s= lua_toLane(L,1);
    enum e_status st= s->status;    // read just once (volatile)
    const char *str;
    
    if (s->mstatus == KILLED)
        st= CANCELLED;

    str= (st==PENDING) ? "pending" :
         (st==RUNNING) ? "running" :    // like in 'co.status()'
         (st==WAITING) ? "waiting" :
         (st==DONE) ? "done" :
         (st==ERROR_ST) ? "error" :
         (st==CANCELLED) ? "cancelled" : NULL;
    ASSERT_L(str);

    lua_pushstring( L, str );
    return 1;
}


//---
// [...] | [nil,err_val]= thread_wait( lane_ud [, wait_secs=-1] )
//
//  timeout:   returns nil
//  done:      returns return values (0..N)
//  error:     returns nil + error value
//  cancelled: returns nil
//
LUAG_FUNC( thread_wait )
{
    struct s_lane *s= lua_toLane(L,1);
    double wait_secs= luaL_optnumber(L,2,-1.0);
    lua_State *L2= s->L;
    int ret;

    bool_t done= 
#if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC) || (defined PTHREAD_TIMEDJOIN)
        THREAD_WAIT( &s->thread, wait_secs );
#else
        THREAD_WAIT( &s->thread, &s->done_signal_, &s->done_lock_, &s->status, wait_secs );
#endif
    if (!done)
        return 0;      // timeout: pushes none, leaves 'L2' alive

    // Thread is DONE/ERROR_ST/CANCELLED; all ours now

    STACK_GROW( L, 1 );

    switch( s->status ) {
        case DONE: {   
            uint_t n= lua_gettop(L2);       // whole L2 stack
            luaG_inter_move( L2,L, n );
            ret= n;
            } break;

        case ERROR_ST:
            lua_pushnil(L);
            luaG_inter_move( L2,L, 1 );    // error message at [-1]
            ret= 2;
            break;

        case CANCELLED:
            ret= 0;
            break;
        
        default:
            fprintf( stderr, "Status: %d\n", s->status );
            ASSERT_L( FALSE ); ret= 0;
    }
    lua_close(L2);

    return ret;
}


/*---=== Timer support ===---
*/

/*
* Push a timer gateway Linda object; only one deep userdata is
* created for this, each lane will get its own proxy.
*
* Note: this needs to be done on the C side; Lua wouldn't be able
*       to even see, when we've been initialized for the very first
*       time (with us, they will be).
*/
static
void push_timer_gateway( lua_State *L ) {

    /* No need to lock; 'static' is just fine
    */
    static DEEP_PRELUDE *p;  // = NULL

  STACK_CHECK(L)
    if (!p) {
        // Create the Linda (only on first time)
        //
        // proxy_ud= deep_userdata( idfunc )
        //
        lua_pushcfunction( L, luaG_deep_userdata );
        lua_pushcfunction( L, LG_linda_id );
        lua_call( L, 1 /*args*/, 1 /*retvals*/ );

        ASSERT_L( lua_isuserdata(L,-1) );
        
        // Proxy userdata contents is only a 'DEEP_PRELUDE*' pointer
        //
        p= * (DEEP_PRELUDE**) lua_touserdata( L, -1 );
        ASSERT_L(p && p->refcount==1 && p->deep);

        // [-1]: proxy for accessing the Linda

    } else {
        /* Push a proxy based on the deep userdata we stored. 
        */
        luaG_push_proxy( L, LG_linda_id, p );
    }
  STACK_END(L,1)
}

/*
* secs= now_secs()
*
* Returns the current time, as seconds (millisecond resolution).
*/
LUAG_FUNC( now_secs )
{
    lua_pushnumber( L, now_secs() );
    return 1;
}

/*
* wakeup_at_secs= wakeup_conv( date_tbl )
*/
LUAG_FUNC( wakeup_conv )
{
    int year, month, day, hour, min, sec, isdst;
    struct tm tm= {0};
        //
        // .year (four digits)
        // .month (1..12)
        // .day (1..31)
        // .hour (0..23)
        // .min (0..59)
        // .sec (0..61)
        // .yday (day of the year)
        // .isdst (daylight saving on/off)

  STACK_CHECK(L)    
    lua_getfield( L, 1, "year" ); year= lua_tointeger(L,-1); lua_pop(L,1);
    lua_getfield( L, 1, "month" ); month= lua_tointeger(L,-1); lua_pop(L,1);
    lua_getfield( L, 1, "day" ); day= lua_tointeger(L,-1); lua_pop(L,1);
    lua_getfield( L, 1, "hour" ); hour= lua_tointeger(L,-1); lua_pop(L,1);
    lua_getfield( L, 1, "min" ); min= lua_tointeger(L,-1); lua_pop(L,1);
    lua_getfield( L, 1, "sec" ); sec= lua_tointeger(L,-1); lua_pop(L,1);

    // If Lua table has '.isdst' we trust that. If it does not, we'll let
    // 'mktime' decide on whether the time is within DST or not (value -1).
    //
    lua_getfield( L, 1, "isdst" );
    isdst= lua_isboolean(L,-1) ? lua_toboolean(L,-1) : -1;
    lua_pop(L,1);
  STACK_END(L,0)

    tm.tm_year= year-1900;
    tm.tm_mon= month-1;     // 0..11
    tm.tm_mday= day;        // 1..31
    tm.tm_hour= hour;       // 0..23
    tm.tm_min= min;         // 0..59
    tm.tm_sec= sec;         // 0..60
    tm.tm_isdst= isdst;     // 0/1/negative

    lua_pushnumber( L, (double) mktime( &tm ) );   // ms=0
    return 1;
}


/*---=== Module linkage ===---
*/

#define REG_FUNC( name ) \
    lua_pushcfunction( L, LG_##name ); \
    lua_setglobal( L, #name )

#define REG_FUNC2( name, val ) \
    lua_pushcfunction( L, val ); \
    lua_setglobal( L, #name )

#define REG_STR2( name, val ) \
    lua_pushstring( L, val ); \
    lua_setglobal( L, #name )

#define REG_INT2( name, val ) \
    lua_pushinteger( L, val ); \
    lua_setglobal( L, #name )


int LUAG_EXPORT luaopen_lanes( lua_State *L ) {
    const char *err;
    static volatile char been_here;  // =0

    // One time initializations:
    //
    if (!been_here) {
        been_here= TRUE;

#if (defined PLATFORM_WIN32) || (defined PLATFORM_POCKETPC)
        now_secs();     // initialize 'now_secs()' internal offset
#endif

#if (defined PLATFORM_OSX) && (defined _UTILBINDTHREADTOCPU)
        chudInitialize();
#endif
    
        // Locks for 'tools.c' inc/dec counters
        //
        MUTEX_INIT( &deep_lock );
        MUTEX_INIT( &mtid_lock );
    
        // Serialize calls to 'require' from now on, also in the primary state
        //
        MUTEX_RECURSIVE_INIT( &require_cs );

        serialize_require( L );

        //---
        // Linux needs SCHED_RR to change thread priorities, and that is only
        // allowed for sudo'ers. SCHED_OTHER (default) has no priorities.
        // SCHED_OTHER threads are always lower priority than SCHED_RR.
        //
        // ^-- those apply to 2.6 kernel.  IF **wishful thinking** these 
        //     constraints will change in the future, non-sudo priorities can 
        //     be enabled also for Linux.
        //
#ifdef PLATFORM_LINUX
        sudo= geteuid()==0;     // we are root?

        // If lower priorities (-2..-1) are wanted, we need to lift the main
        // thread to SCHED_RR and 50 (medium) level. Otherwise, we're always below 
        // the launched threads (even -2).
	    //
  #ifdef LINUX_SCHED_RR
        if (sudo) {
            struct sched_param sp= {0}; sp.sched_priority= _PRIO_0;
            PT_CALL( pthread_setschedparam( pthread_self(), SCHED_RR, &sp) );
        }
  #endif
#endif
        err= init_keepers();
        if (err) 
            luaL_error( L, "Unable to initialize: %s", err );
    }
    
    // Linda identity function
    //
    REG_FUNC( linda_id );

    // metatable for threads
    //
    lua_newtable( L );
    lua_pushcfunction( L, LG_thread_gc );
    lua_setfield( L, -2, "__gc" );

    lua_pushcclosure( L, LG_thread_new, 1 );    // metatable as closure param
    lua_setglobal( L, "thread_new" );

    REG_FUNC( thread_status );
    REG_FUNC( thread_wait );
    REG_FUNC( thread_cancel );

    REG_STR2( _version, VERSION );
    REG_FUNC( _single );

    REG_FUNC2( _deep_userdata, luaG_deep_userdata );

    REG_FUNC( now_secs );
    REG_FUNC( wakeup_conv );

    push_timer_gateway(L);    
    lua_setglobal( L, "timer_gateway" );

    REG_INT2( max_prio, THREAD_PRIO_MAX );

    lua_pushlightuserdata( L, CANCEL_ERROR );
    lua_setglobal( L, "cancel_error" );

    return 0;
}


