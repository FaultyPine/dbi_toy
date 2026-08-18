

trying to learn how to write a DBI engine


currently supports:
- injecting into a target process/thread, or waiting for an exe with a given name to spawn
- relocating rip-relative instructions
- emitting basic block terminators that dispatch back into the DBI
    - not all terminators are supported yet, that is the current WIP


goal: i consider this complete once i'm able to
- count basic blocks executed in a program
- automatically emit some arbitrary code at the beginning/end of all function calls
    - ideally, i'd like the "done" version of this toy to be able to call Tracy profiler on all functions in any program automatically. Maybe even a PDB parser that would map the original PC to the right symbol and emit the correct profiler block names.