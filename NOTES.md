Notes to myself as i learn about DBI stuff


mac/linux have a "red zone" which is ~128 bytes after the stack pointer that are reserved for "leaf functions" (funcs that don't call other funcs)
so those leaf funcs have a place to put scratch stuff
windows doesn't have that so anything after sp (stack pointer) is potentially volatile/important

"rip-relative". We can't just load a 64bit pointer of a global
there's no way to express that in x64, so a lea (load effective address, basically C++ &someVar) 
must be a 32bit address, but our module (and therefore the global) may be loaded outside the 
4GB range a 32bit number could express. So we need to compute a 32bit address of this global
so we use the current instruction pointer (rip) as a base and then offset from that to the global. 



Windows abi:
stack ptr needs 16 byte alignment before a fn call
there is always 32 bytes of "shadow space" on the stack before calling a function. even if the function doesn't use it.
rcx, rdx, r8, r9 are first integer arguments to a func

volatile regs: regs that may change after a function call
non-volatile regs: regs that the callee makes sure are preserved across fn calls
Volatile (Caller-saved): RAX, RCX, RDX, R8, R9, R10, R11, and XMM0–XMM5
Non-volatile (Callee-saved): RBX, RBP, RDI, RSI, RSP, R12, R13, R14, R15, and XMM6–XMM15
