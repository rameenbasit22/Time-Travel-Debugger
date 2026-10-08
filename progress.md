# Time-Travel Debugger Progress

## Phase 01

### Day 1 (4-10-2026)
#  Project Setup 

- Created the project repository.
- Added the server.cpp template.
- Added README.md.
- Added progress.md.
- Added Makefile.
- Added source.bin for input.
- Set up the project in Ubuntu.
- Initialized Git repository.
# Implementing the custom Stack class
- I implemented all basic functions like push, pop, peek, isEmpty, depth and a little bit complex function of snapshot_into as well
### Day 2 (5-10-2026)
#  Pass 0 

- Implemented source file reading.
- Added first-word and second-word parsing.
- Implemented FUNC and FUNC_END validation.
- Added nested function detection.
- Added missing FUNC_END detection.
- Added invalid FUNC_END detection.
- Added comment line handling.
- Tested valid and invalid C-- programs.

### Day 3 (6-10-2026)
# Pass 1
- Implemented the functions: writeResolveRecord and readResolveRecord
- While working on resolveProgram, faced some errors while running the main which after debugging in gdb showed that there were error in firstWord() and secondWord() functions so fixed them again and then ran ./server again and results were correct this time.

### Day 4 (7-10-2026)
# Pass 2
- Implemented the Timeline class
- Done with the tokenizeLine() function and also corrected a wrong variable name in pass 1 as it gave error

### Day 5 (8-10-2026)
# Pass 2
- Continued with implementation of executeProgram()
- To avoid making the function complex I divided it into smaller parts like set, add etc variable.
- Did the call to jump to required function
- Did func end to return to calling function
- It took a lot of time as it was complex but still ended with testing it and everything worked properly
# Pass 3
- Started Pass 3 with placing struct TTDB and placeholders in writeHeader
- Made some helping functions for writing snapshots to serialize strings and variables 

### Status
Pass 2 is now completed and Pass 3 is in progress.

### Next Step

Implement Pass 3 remaining functions.

