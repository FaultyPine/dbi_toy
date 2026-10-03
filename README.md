
Only for x64 windows.
Building: run `build.bat`

trying to learn how to write a DBI engine


currently supports:
- injecting into a target process/thread, or waiting for an exe with a given name to spawn
- relocating rip-relative instructions
- emitting basic block terminators that dispatch back into the DBI
- backpatching code cache blocks
