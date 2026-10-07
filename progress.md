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
### Status

Pass 1 Resolve completed and tested and Pass 2 is in progress.

### Next Step

Implement Pass 2 Execution.

