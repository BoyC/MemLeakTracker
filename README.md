# MemLeakTracker
A single file drop-in memory leak tracking solution for C++ on Windows

This small piece of code allows for global memory leak tracking on Windows.

Use: Simply place this CPP in your solution and have it compile with the rest
of the code. A to-the-point memory leak report will be presented in the debug
output upon exit.

This code works by overriding the global new and delete operators so if your 
project already did that there will be some linker errors to resolve.

There is a performance hit associated with using this code which is why the
default configuration disables tracking for release builds. If you only
want to see if there are memory leaks at all the stack tracing can be disabled
to improve performance.

Limitations: this code only tracks new and delete calls in their many forms.
malloc/HeapAlloc/etc functions aren't tracked but can be easily added through
proxy calls - see the bottom of the CPP for the new/delete implementations.
The tracking in this code will also not see through dll boundaries.

May it serve you as well as it did me.

