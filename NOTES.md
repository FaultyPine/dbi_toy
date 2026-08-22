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



https://github.com/zyantific/zydis/blob/master/examples/RewriteCode.c - how to decode, change, reencode instructions
https://github.com/zyantific/zydis/blob/master/examples/EncodeFromScratch.c  - very small easy to read "jit" code execution
zydis points out in their readme that this is a nice zydis wrapper (but uses c++) https://github.com/zyantific/zasm/tree/master

I should use this!  https://github.com/LuaJIT/LuaJIT/tree/v2.1/dynasm
                really clever C machine code emitter where you can kinda put emit inline assembly directly inside C



Simple way to keep control of a thread in a DBI is to emit "exits" that still jump into our code cache.
a simple "exit" there means saving all registers, branching to our C code, doing a code cache lookup (or compile, if it's not cached), restoring registers, and jumping to that code cache.
But that's slow, so we implement "backpatching". Instead of doing the register save + C call + restore + jump. Once we know we have a code cache block compiled for the target address of the jump,
we patch the exit to jump directly to our known existing code cache block. 

Though the above becomes a little more complex with indirect jumps/calls. Because those rely on runtime memory addresses rather than static ones we see in the code.
For those, i've heard a little "inline" cache of the previous jump is good. I.E. the jump instruction jumps to the value of the next 8 bytes after the jump instruction (rip-relative)
and we populate those 8 bytes with what we think it'll be, then we emit a cmp to see if the target address of the actual code matches our cached codecache address, and if so we just go there.
If it's not the cached one, we need to do the slow exit.


One thing i was told about is if you're wanting to do some instrumentation is DBI engines will "reserve" some cpu reigsters for themselves.
I.E. you reserve r15 to always hold a pointer to some per-thread state context struct. And maybe r14 for "scratch" work.
Then whenever the real program tries to use the registers you've "reserved", you instead redirect them to "virtual" registers you store in the per-thread ctx.
So like if you reserve r15 as the thread context ptr...
```
add rax, r15
```
becomes
```
add rax, [r15 + guest_r15_offset]
```
A problem arises when you need more temp/scratch storage...
Imagine you've reserved r15 as threadctx ptr, and r14 as your own perma scratch reg.
So all ops with r15 and r14 must go through "virtual" regs.
Then
```
add r14, r15
```
is an issue because we need some scratch storage to handle it.
One way to solve it is to have some scratch storage on your thread ctx ptr and save/restore the scratch regs... 
so we map
r14 => rax
r15 => rbx
so now rax and rbc are our scratch regs that represent the real r14 and r15
so we emit
```
// save real rax/rbc off
mov [r14 + slot1], rax
mov [r14 + slot2], rbx
// load rax/rbx up with the real r14/15
mov rax, [r14 + r14_offset]
mov rbx, [r14 + r15_offset]
// actual op
add rax, rbx
// save result of the op into virtual regs
mov [r14 + r14_offset], rax
mov [r14 + r15_offset], rbx
// restore our scratch regs
mov rax, [r14 + slot1]
mov rbx, [r14 + slot2]
```

^ is a simple and slower way of doing it.
A more complex but faster way is to do real dataflow analysis of live/dead registers, to decide which ones are available for scratch work and use those.
"A guest register is live at a point if its current value may be read later before being overwritten." If <- is not the case, the reg is "dead" and we can just use it as scratch.
