
Only for x64 windows.
Building: run `build.bat`. Or `build.bat release` for an optimized build
trying to learn how to write a DBI engine


currently supports:
- injecting into a target process/thread, or waiting for an exe with a given name to spawn
- relocating rip-relative instructions
- emitting basic block terminators that dispatch back into the DBI
- backpatching code cache blocks (direct and indirect branches)

TODO:
- use libpdb to find the real start/end of function code. Excluding embedded data like jump tables
- split up the code cache to be allocated near the code modules
- handle indirect branches better
    - in a benchmark of compiling clang under this dbi, most of the time was spent on cache misses for returns
        - need a bigger cache. dynamorio uses an open-addressed linear probe hashtable shared across threads.
    - also, the indirect branch cache should live in data, not in exe memory

- idea:
    - dynamorio's goal is to instrument arbitrary programs, while remaining completely transparent. This makes things difficult.
    - we could probably make a much more performant JIT by relaxing our constraints and allowing us to enforce rules about which programs are supported.
    - Like, we could require available symbols. That way, we could determine where all the relevant functions are in a module, and pre-jit it before hijacking a thread.
    - We also don't need perfect transparency, so instead of a hashtable cache for returns, we could emit an actual return directly to code cache.
        - dynamorio paper said poor indirect branch handling for returns can result in 20% perf hit. Using raw return can much more closely mimic perf of a native program.
        - would mean some stuff could break. Like programs using __ReturnAddress intrinsic. 
    - we will not support arbitrary instrumentation growth. 
    - we will not support attaching to a program during execution.
        - The catch here is we WILL support toggling certain instrumentation features.
        - So you'd always boot under the dbi, but initially it wouldn't do anything special, and in theory could have very near-native perf.
        - then only when you want to enable a feature of the dbi, it would then pause all threads, patch stubs/nops in the code cache to enable that feature, and resume threads.
        - so i.e. you'd boot your game your working on not like `game.exe my_args`, instead like `dbi.exe game.exe my_args` -> the dbi launches your process suspended -> compiles a new module for your code that contains nops in fixed places -> loads it -> hijacks the suspended thread(s) you care about into the new module -> run as normal.... user toggles "coverage tracing" == patch nops with the instrumentation feature itself or jmp's to the feature if it's too big to put it inline
        - this way, we can also load the instrumented code as a real "module" and even give it it's own pdb symbols that resolve to the real program's info!
            - copy real program's pdb for that module, then it's a matter of remapping the source lines i think? maybe locals too...

DynamoRIO phd paper, lots of extremely valuable insights here
https://www.burningcutlery.com/derek/docs/phd.pdf
