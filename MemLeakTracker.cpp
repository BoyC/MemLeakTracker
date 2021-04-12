/*
Copyright (c) 2021 Barna 'BoyC' Buza - https://github.com/BoyC/MemLeakTracker

Permission is hereby granted, free of charge, to any person obtaining a copy of 
this software and associated documentation files (the "Software"), to deal in 
the Software without restriction, including without limitation the rights to 
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies 
of the Software, and to permit persons to whom the Software is furnished to do 
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all 
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, 
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
SOFTWARE.
*/

/*

This small piece of code allows for global memory leak tracking on Windows.

Use: Simply place this CPP in your solution and have it compile with the rest
of the code. A to-the-point memory leak report will be presented in the debug
output upon exit.

This code works by overriding the global new and delete operators so if your 
project already did that there will be some linker errors to resolve.

There is a performance hit associated with using this code which is why the
default configuration below disables tracking for release builds. If you only
want to see if there are memory leaks at all the stack tracing can be disabled
to improve performance.

Limitations: this code only tracks new and delete calls in their many forms.
malloc/HeapAlloc/etc functions aren't tracked but can be easily added through
proxy calls - see the bottom of this file for the new/delete implementations.
The tracking in this code will also not see through dll boundaries.

May it serve you as well as it did me.

*/

//////////////////////////////////////////////////////////////////////////
// Config

#define ENABLE_MEMLEAK_TRACKING_IN_DEBUG   1
#define ENABLE_STACK_TRACES_IN_DEBUG       1

#define ENABLE_MEMLEAK_TRACKING_IN_RELEASE 0
#define ENABLE_STACK_TRACES_IN_RELEASE     0

#define STACK_TRACE_DEPTH 10

//////////////////////////////////////////////////////////////////////////
// Auto config

#ifndef _DEBUG
#if ENABLE_MEMLEAK_TRACKING_IN_RELEASE
#define ENABLE_MEMORY_LEAK_TRACKING
#endif // ENABLE_MEMLEAK_TRACKING_IN_RELEASE
#if ENABLE_STACK_TRACES_IN_RELEASE
#define ENABLE_STACK_TRACE
#endif // ENABLE_STACK_TRACES_IN_RELEASE
#define STACK_OFFSET 1
#else
#if ENABLE_MEMLEAK_TRACKING_IN_DEBUG
#define ENABLE_MEMORY_LEAK_TRACKING
#endif // ENABLE_MEMLEAK_TRACKING_IN_DEBUG
#if ENABLE_STACK_TRACES_IN_DEBUG
#define ENABLE_STACK_TRACE
#endif // ENABLE_STACK_TRACES_IN_DEBUG
#define STACK_OFFSET 4
#endif // _DEBUG

//////////////////////////////////////////////////////////////////////////
// Implementation

#ifdef ENABLE_MEMORY_LEAK_TRACKING

#include <unordered_map>
#include <Windows.h>
#include <tchar.h>

#ifdef ENABLE_STACK_TRACE
#include <DbgHelp.h>
#pragma comment(lib,"dbghelp.lib")
#endif // ENABLE_STACK_TRACE

namespace LeakTracker
{

#ifdef ENABLE_STACK_TRACE
class StackTracker
{
  void* stack[ STACK_TRACE_DEPTH ];
  static bool dbgInitialized;

  static void InitializeSym()
  {
    if ( !dbgInitialized )
    {
      SymInitialize( GetCurrentProcess(), NULL, true );
      SymSetOptions( SYMOPT_LOAD_LINES );
      dbgInitialized = true;
    }
  }
public:

  StackTracker()
  {
    memset( stack, 0, sizeof( stack ) );
    RtlCaptureStackBackTrace( STACK_OFFSET, STACK_TRACE_DEPTH, stack, NULL );
  }

  void DumpToDebugOutput()
  {
    InitializeSym();

    TCHAR buffer[ 1024 ];
    memset( buffer, 0, sizeof( buffer ) );

    for ( int x = 0; x < STACK_TRACE_DEPTH; x++ )
    {
      if ( stack[ x ] )
      {
        DWORD  dwDisplacement;
        IMAGEHLP_LINE line;

        line.SizeOfStruct = sizeof( IMAGEHLP_LINE );

        bool addressFound;
#ifndef _WIN64
        addressFound = SymGetLineFromAddr( GetCurrentProcess(), (DWORD)stack[ x ], &dwDisplacement, &line );
#else
        addressFound = SymGetLineFromAddr64( GetCurrentProcess(), (DWORD64)stack[ x ], &dwDisplacement, &line );
#endif // _WIN64

        if ( addressFound )
          _sntprintf_s( buffer, 1023, _T( "\t\t%hs (%d)\n\0" ), line.FileName, line.LineNumber );
        else
          _sntprintf_s( buffer, 1023, _T( "\t\tUnresolved address: %p\n\0" ), stack[ x ] );
        OutputDebugString( buffer );
      }
    }
    OutputDebugString( _T( "\n" ) );
  }
};

bool StackTracker::dbgInitialized = false;
#endif // ENABLE_STACK_TRACE

class Mutex
{
  friend class Lock;
  CRITICAL_SECTION critSec;

public:

  Mutex()
  {
    InitializeCriticalSectionAndSpinCount( &critSec, 0x100 );
  }
  ~Mutex()
  {
    DeleteCriticalSection( &critSec );
  }
  CRITICAL_SECTION& GetCriticalSection()
  {
    return critSec;
  }
};

class Lock
{
  const LPCRITICAL_SECTION critSec;
public:

  Lock( Mutex& mutex )
    : critSec( &mutex.critSec )
  {
    EnterCriticalSection( critSec );
  }

  ~Lock()
  {
    LeaveCriticalSection( critSec );
  }
};

class AllocationInfo
{
public:
  size_t size;

#ifdef ENABLE_STACK_TRACE
  StackTracker stack;
#endif // ENABLE_STACK_TRACE

  AllocationInfo( size_t size )
    : size( size )
  {
  }
};

class MemTracker
{
  Mutex critsec;
  bool paused = true; // this needs to be above the memTrackerPool variable (init order)
  std::unordered_map<const void*, AllocationInfo> memTrackerPool;

public:

  MemTracker()
  {
    paused = false;
  }

  ~MemTracker()
  {
    paused = true;

    if ( memTrackerPool.size() )
    {
      //report leaks
      OutputDebugString( _T( "\n--- Memleaks start here ---\n\n" ) );

      size_t totalLeaked = 0;

      TCHAR buffer[ 1024 ];

      for ( auto& entry : memTrackerPool )
      {
        _sntprintf_s( buffer, 1023, _T( "Leak: %zu bytes\n\0" ), entry.second.size );
        OutputDebugString( buffer );

#ifdef ENABLE_STACK_TRACE
        entry.second.stack.DumpToDebugOutput();
#endif
        totalLeaked += entry.second.size;
      }

      _sntprintf_s( buffer, 1023, _T( "\tTotal bytes leaked: %zu\n\n\0" ), totalLeaked );
      OutputDebugString( buffer );
    }
    else
    {
      OutputDebugString( _T( "**********************************************************\n\t\t\t\t\tNo memleaks found.\n**********************************************************\n\n" ) );
    }
  }

  void AddPointer( void* p, size_t size )
  {
    Lock cs( critsec );
    if ( !paused && p )
    {
      paused = true;
      memTrackerPool.insert_or_assign( p, AllocationInfo( size ) );
      paused = false;
    }
  }

  void RemovePointer( void* p )
  {
    Lock cs( critsec );
    if ( !paused && p )
    {
      paused = true;
      size_t erased = memTrackerPool.erase( p );
      if ( !erased )
      {
        OutputDebugString( _T( "**** ERROR: Trying to delete non logged, possibly already freed memory block!\n" ) );
#ifdef ENABLE_STACK_TRACE
        StackTracker s;
        s.DumpToDebugOutput();
#endif // ENABLE_STACK_TRACKER
      }
      paused = false;
    }
  }

  void Pause()
  {
    Lock cs( critsec );
    paused = true;
  }

  void Resume()
  {
    Lock cs( critsec );
    paused = false;
  }
};

//This should force the memTracker variable to be constructed before everything else:
#pragma warning(disable:4074)
#pragma init_seg(compiler)
MemTracker memTracker;
}

void* __cdecl operator new( size_t size )
{
  void* p = malloc( size );
  LeakTracker::memTracker.AddPointer( p, size );
  return p;
}

void* __cdecl operator new[]( size_t size )
{
  void* p = malloc( size );
  LeakTracker::memTracker.AddPointer( p, size );
  return p;
}

void* __cdecl operator new( size_t size, const char* file, int line )
{
  void* p = malloc( size );
  LeakTracker::memTracker.AddPointer( p, size );
  return p;
}

void* __cdecl operator new[]( size_t size, const char* file, int line )
{
  void* p = malloc( size );
  LeakTracker::memTracker.AddPointer( p, size );
  return p;
}

void __cdecl operator delete( void* pointer )
{
  LeakTracker::memTracker.RemovePointer( pointer );
  free( pointer );
}

void __cdecl operator delete[]( void* pointer )
{
  LeakTracker::memTracker.RemovePointer( pointer );
  free( pointer );
}

void __cdecl operator delete( void* pointer, const char* file, int line )
{
  LeakTracker::memTracker.RemovePointer( pointer );
  free( pointer );
}

void __cdecl operator delete[]( void* pointer, const char* file, int line )
{
  LeakTracker::memTracker.RemovePointer( pointer );
  free( pointer );
}

#endif // ENABLE_MEMORY_LEAK_TRACKING