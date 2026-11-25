
Only for x64 windows.
Building: run `build.bat`. Or `build.bat release` for an optimized build
trying to learn how to write a DBI engine


tech:
- injecting into a target process/thread
- relocating rip-relative instructions
- emitting basic block terminators that dispatch back into the DBI
- backpatching code cache blocks (direct and indirect branches)
- shared mem communication between the main "control" process, and the target app instrumented process
- exceptions print the surrounding code cache & original app instructions for comparison

TODO:
- handle indirect branches better
    - need a bigger cache. dynamorio uses an open-addressed linear probe hashtable shared across threads.
- experiment with native call/ret, since we can guarantee we will hijack the thread before app code runs
- split up the code cache to be allocated near the code modules



DynamoRIO phd paper, lots of extremely valuable insights here
https://www.burningcutlery.com/derek/docs/phd.pdf
