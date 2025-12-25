# Collaborative Editor (Lock-free CRDT-based)
```
Name:   Narendra Kumar
Rollno: 25CS60R18
```
## Files:
- proj.cpp        : Source code (modified for lock-free operation)
- README.txt        : This file
- DESIGNDOC.txt     : Design document describing architecture and decisions
---

## Compilation Instructions:
### 1. Install dependencies (Ubuntu):
   `sudo apt-get update`   

### 2. Compile with g++ (C++11), link rt for message queues and pthreads:
   ```bash
   g++ -std=c++11 proj.cpp -o etr -pthread -lrt
```
## Execution Instructions:
- Start multiple terminals (3+). In each terminal run:
  ./editor user_1
  ./editor user_2
  ./editor user_3
- Each invocation creates a local file: user_<id>_doc.txt and sets up message queue /shared memory.

## Dependencies:
- g++ with C++11 support
- pthreads (-pthread)
- librt (-lrt) for POSIX message queues
- POSIX-compliant OS (tested on Ubuntu 20.04)

### How to Test (basic):
1. Start three terminals and run ./editor user_1, ./editor user_2, ./editor user_3 respectively.
2. Edit files user_1_doc.txt, user_2_doc.txt in any editor and save.
3. Changes will be detected and broadcast; merge happens every batch interval (default in code).
4. Observe convergence and messages printed on terminals.

